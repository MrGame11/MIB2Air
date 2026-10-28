"""Collect an explicit set of stock inputs into a local, verified ZIP."""

import datetime
import json
import os
import re
import tempfile
import zipfile
from pathlib import Path

from installer.ap_profile import AP_PATH
from installer.ap_profile import MODE_PATH as AP_MODE_PATH
from installer.elf import require, sha256
from installer.firmware import INPUTS, source_digest
from installer.paths import BASE, REMOTE, ROOT, json_bytes
from installer.progress import detail, step, summary
from installer.result import Result
from installer.snapshot import download, save_unprofiled

# Values a profile port needs to compare; every other AP value is replaced.
AP_PUBLIC_TAGS = {b"Security", b"HideSSID", b"Channel"}


def redact_ap(document):
    """Keep the AP XML structure for porting; never keep SSID, key or unknown values."""
    tokens = re.split(rb"(<[^>]*>)", document)
    for index, token in enumerate(tokens):
        if token.startswith(b"<!"):
            # Comments and CDATA can carry values; keep only that they existed.
            tokens[index] = b"<!REDACTED>"
        elif token.startswith(b"<"):
            if not token.startswith(b"<?"):
                # Attribute values are unexpected in the native schema.
                tokens[index] = re.sub(
                    rb"=\s*(\"[^\"]*\"|'[^']*')", b'="REDACTED"', token
                )
        elif token.strip():
            opening = tokens[index - 1] if index else b""
            if opening[1:-1] not in AP_PUBLIC_TAGS:
                tokens[index] = b"REDACTED"
    return b"".join(tokens)


def context(device, directory):
    """Device formats a profile port must check, without credentials."""
    directory = Path(directory)
    sources = {
        "wlanapconfig.xml": (AP_PATH, redact_ap),
        "wlanlastmode.conf": (AP_MODE_PATH, None),
    }
    captured = {}
    for name, (remote, transform) in sources.items():
        detail("Reading " + name)
        if not device.exists(remote):
            captured[name] = {"source": remote, "missing": True}
            continue
        data = device.read(remote)
        if transform:
            data = transform(data)
        captured[name] = {"source": remote, "data": data}
    detail("Reading mount table")
    captured["mount.txt"] = {"source": "mount", "data": device.command("mount")}
    record = {}
    for name, entry in captured.items():
        data = entry.pop("data", None)
        if data is not None:
            path = directory / "device" / name
            path.parent.mkdir(mode=0o700, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o600)
            entry["sha256"] = sha256(data)
        record[name] = entry
    (directory / "device.json").write_bytes(json_bytes(record))


def archive(directory, *, reason=None):
    directory = Path(directory)
    record = json.loads((directory / "snapshot.json").read_text())
    require(set(record["files"]) == set(REMOTE), "Incomplete stock capture")
    manifest = {
        "schema": 1,
        "purpose": "stock-data-capture",
        "created": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "collector_source_sha256": source_digest(),
        "reason": reason or "Manual stock data collection",
        "files": {},
        "device": {},
        "notice": "Current startup/firewall configuration may contain custom settings. AP SSID and key are replaced with REDACTED. Bluetooth pairing data and system logs are not collected.",
    }
    extra = directory / "device.json"
    device_files = json.loads(extra.read_text()) if extra.is_file() else {}
    destination = directory / "stock-data.zip"
    fd, temporary = tempfile.mkstemp(prefix=".stock-", dir=directory)
    try:
        with os.fdopen(fd, "w+b") as stream:
            with zipfile.ZipFile(
                stream, "w", compression=zipfile.ZIP_DEFLATED
            ) as bundle:

                def add(relative, expected, kind, name):
                    """Archive a captured file only if it still has its recorded hash."""
                    path = directory / relative
                    require(
                        path.is_file() and not path.is_symlink(),
                        f"Invalid {kind}: {name}",
                    )
                    data = path.read_bytes()
                    require(
                        sha256(data) == expected, f"{kind.capitalize()} changed: {name}"
                    )
                    bundle.writestr(relative, data)
                    return data

                for name, remote in REMOTE.items():
                    relative = "inputs/" + INPUTS.get(name, "config/" + name)
                    data = add(
                        relative, record["files"][name]["sha256"], "stock input", name
                    )
                    manifest["files"][name] = {
                        **record["files"][name],
                        "path": relative,
                        "remote": remote,
                        "bytes": len(data),
                    }
                for name, entry in device_files.items():
                    manifest["device"][name] = entry
                    if not entry.get("missing"):
                        relative = "device/" + name
                        add(relative, entry["sha256"], "device context", name)
                        manifest["device"][name] = dict(entry, path=relative)
                bundle.writestr("manifest.json", json_bytes(manifest))
            stream.flush()
            os.fsync(stream.fileno())
        # Publish a complete archive atomically; never replace an existing one.
        os.link(temporary, destination)
    finally:
        os.unlink(temporary)
    summary(f"{len(REMOTE)} files archived")
    shown = (
        destination.relative_to(ROOT)
        if destination.is_relative_to(ROOT)
        else destination
    )
    return Result(
        "unsupported_firmware" if reason else "stock_collected",
        "Firmware is not supported. Installation stopped."
        if reason
        else "Stock data collected.",
        (f"Archive: {shown}",),
    )


def collect(device, directory):
    step("Collect stock data")
    require(
        not device.exists(BASE),
        "Remove the existing Wireless AA installation before collecting stock data",
    )
    records, _, _ = download(device, directory, reuse=False)
    save_unprofiled(device, directory, records)
    context(device, directory)
    step("Create stock archive")
    return archive(directory)
