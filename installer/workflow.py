"""Durable installation/removal journal. All patching happens on the host in Docker."""

import json
import os
import re
import secrets
from pathlib import Path

from installer import configuration
from installer.build import FILES, build_package, new_release
from installer.device import EXECUTABLE, PRIVATE, writable_root
from installer.elf import Refuse, is_sha256, require, sha256
from installer.firmware import UnsupportedFirmware, profiles
from installer.paths import (
    BASE,
    CONFIGS,
    CURRENT,
    PATCHED,
    RECOVERY,
    RELEASE_ID,
    RELEASES,
    REMOTE,
    json_bytes,
    release_dir,
    release_file,
)
from installer.progress import detail, item_done, outline, size, step, summary
from installer.progress import item as progress_item
from installer.result import Result
from installer.snapshot import capture, unchanged
from installer.stock import archive as stock_archive
from installer.stock import context as stock_context

JOURNAL = BASE + "/install.json"
LARGE = ("wa", "sal", "libc", "gal")
# Process-list markers for the patched services and the recovery image.
RUNNING_PATCHED = tuple(("/" + name).encode() for name in PATCHED.values())
RUNNING_RECOVERY = ("/" + RECOVERY).encode()


def saved(device, record):
    flags = getattr(device, "_root_mount_flags", None)
    if type(flags) is int:
        record.setdefault("root_mount_flags", flags)
    device.write(JOURNAL, json_bytes(record))


def load(device):
    require(device.attrs(JOURNAL) == PRIVATE, "Installation journal attributes changed")
    try:
        record = validate_record(json.loads(device.read(JOURNAL)))
        if "root_mount_flags" in record:
            device._root_mount_flags = record["root_mount_flags"]
        return record
    except (KeyError, TypeError, AttributeError) as error:
        raise ValueError("Malformed installation journal") from error


def validate_record(record):
    require(
        record.get("schema") == 3
        and record.get("profile") in {profile["id"] for profile in profiles()},
        "Unsupported installation journal",
    )
    require(valid_release(record["release"]), "Invalid release in journal")
    if "previous" in record:
        require(valid_release(record["previous"]), "Invalid release in journal")
    require(
        record["root_mode"] in ("ro", "rw") and type(record["may_run"]) is bool,
        "Invalid installation journal",
    )
    if "root_mount_flags" in record:
        flags = record["root_mount_flags"]
        require(
            type(flags) is int
            and flags & ~0x2D == 0
            and bool(flags & 1) == (record["root_mode"] == "ro"),
            "Invalid original root filesystem flags",
        )
    require(set(record["originals"]) == set(REMOTE), "Incomplete original file record")
    for item in record["originals"].values():
        require(is_sha256(item["sha256"]), "Invalid original hash")
        attrs = item["attrs"]
        require(
            set(attrs) == {"uid", "gid", "mode"}
            and all(type(v) is int and v >= 0 for v in attrs.values())
            and attrs["mode"] <= 0o777,
            "Invalid original attributes",
        )
    valid_files(record["files"])
    require(
        record["phase"]
        in (
            "preparing",
            "publishing",
            "installed",
            "updating",
            "recovering",
            "restoring",
            "cleaning",
        ),
        "Unknown installation phase",
    )
    if "token" in record:
        require(
            re.fullmatch(r"[0-9a-f]{32}", record["token"]) is not None,
            "Invalid recovery token",
        )
    if record["phase"] == "recovering" or (
        record["may_run"] and record["phase"] in ("restoring", "cleaning")
    ):
        require("token" in record, "Recovery attempt missing from journal")
    if "restored" in record:
        require(
            set(record["restored"]) == set(CONFIGS)
            and all(is_sha256(h) for h in record["restored"].values()),
            "Invalid restoration hashes",
        )
    require(
        record["phase"] != "cleaning" or "restored" in record,
        "Missing restoration plan",
    )
    require(
        (record["phase"] == "updating") == ("next" in record),
        "Update state does not match journal phase",
    )
    if "next" in record:
        upcoming = record["next"]
        require(
            set(upcoming) == {"release", "files"}
            and valid_release(upcoming["release"])
            and upcoming["release"] not in (record["release"], record.get("previous")),
            "Invalid pending release",
        )
        valid_files(upcoming["files"])
    return record


def valid_release(value):
    return type(value) is str and re.fullmatch(RELEASE_ID, value) is not None


def valid_files(files):
    require(set(files) == set(FILES) | set(CONFIGS), "Invalid installed file list")
    for value in files.values():
        require(is_sha256(value), "Invalid installed hash")


def payload_attrs(name):
    return PRIVATE if name == "cpu.conf" else EXECUTABLE


def recheck(device, record, names):
    unchanged(device, {n: record["originals"][n] for n in names})


def require_workspace(directory):
    require(
        Path(os.environ.get("WAA_WORKDIR", "")).resolve() == directory.resolve(),
        "Build workspace must match the captured snapshot",
    )


def running(processes, names):
    return any(name in processes for name in names)


def check_payload(device, record, release=None, files=None):
    release = release or record["release"]
    files = files or record["files"]
    for name in FILES:
        detail("Checking installed " + name + " on MIB (SHA-256)")
        path = release_file(release, name)
        require(
            device.sha256(path) == files[name],
            "Installed file changed: " + name,
        )
        require(
            device.attrs(path) == payload_attrs(name),
            "Installed attributes changed: " + name,
        )


def unfinished_initialization(device, directory):
    if not device.exists(BASE) or device.exists(JOURNAL):
        return
    require(
        device.entries() <= {BASE, JOURNAL + ".waa-new"},
        "Installation directory has no journal; preserve it for inspection",
    )
    require(
        BASE.encode() not in device.read(REMOTE["startup"])
        and BASE.encode() not in device.processes(),
        "Unjournaled installation may be active",
    )
    if device.exists(JOURNAL + ".waa-new"):
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        (directory / "interrupted-journal.json").write_bytes(
            device.read(JOURNAL + ".waa-new")
        )
    device.cleanup()


def running_releases(device):
    processes = device.processes()
    return {
        line[len(RELEASES) + 1 :].split("/", 1)[0]
        for line in processes.decode("ascii", "replace").split()
        if line.startswith(RELEASES + "/")
    }


def prune(device, record):
    """Drop releases that are neither active, the rollback copy, nor still running."""
    keep = {record["release"], record.get("previous")} - {None}
    device.cleanup(keep=keep | running_releases(device))


def finish_update(device, record):
    upcoming = record.pop("next")
    record.update(
        previous=record["release"],
        release=upcoming["release"],
        files=upcoming["files"],
        phase="installed",
    )
    saved(device, record)
    prune(device, record)


def settle_update(device, record):
    """Complete a switched update or discard an unswitched one."""
    if record["phase"] != "updating":
        return record
    upcoming = record["next"]["release"]
    if device.exists(CURRENT) and device.read(CURRENT) == (upcoming + "\n").encode():
        finish_update(device, record)
    else:
        del record["next"]
        record["phase"] = "installed"
        saved(device, record)
        prune(device, record)
    return record


def check_installed(device, record):
    check_payload(device, record)
    recheck(device, record, (*LARGE, "cpu.conf"))
    require(
        device.exists(BASE + "/mode") and device.read(BASE + "/mode") == b"normal\n",
        "Enablement interrupted or removal requested; run remove",
    )
    require(
        device.exists(CURRENT)
        and device.read(CURRENT) == (record["release"] + "\n").encode(),
        "Active release differs from the journal; run remove",
    )
    for name in CONFIGS:
        require(
            sha256(device.read(REMOTE[name])) == record["files"][name],
            "Installed config changed: " + name,
        )


def update(device, directory, record):
    """Replace the installed release beside the running one; one reboot applies it."""
    step("Download originals", index=1, total=5)
    try:
        capture(device, directory)
    except UnsupportedFirmware as error:
        raise Refuse(
            f"This version cannot build for the installed firmware ({error}); "
            "run remove first"
        ) from None
    # Build from the pre-installation configs; the live ones contain our blocks.
    for name in CONFIGS:
        data = device.read(BASE + "/backup/" + name)
        require(
            sha256(data) == record["originals"][name]["sha256"],
            "Backup damaged: " + name,
        )
        (directory / "inputs/config" / name).write_bytes(data)
    require_workspace(directory)
    step("Build package", index=2, total=5)
    manifest = build_package(directory / "package-installed", record["release"])
    if manifest["files"] == record["files"]:
        summary("same version")
        return Result("already_installed", "Already up to date; no files changed.")
    require(
        all(manifest["files"][n] == record["files"][n] for n in CONFIGS),
        "This version changes startup or firewall configuration; run remove, then install",
    )
    release = new_release()
    package = directory / "package"
    manifest = build_package(package, release, compile=False)
    require(
        not device.exists(release_dir(release))
        and release not in (record["release"], record.get("previous")),
        "Release directory already exists",
    )
    summary(f"{len(FILES)} files verified")
    step("Recheck originals", index=3, total=5)
    recheck(device, record, (*LARGE, "cpu.conf"))
    device.space(
        "/tsd/var",
        sum((package / n).stat().st_size for n in FILES) * 2 + 4 * 1024 * 1024,
    )
    summary("unchanged · space ok")
    step("Upload new version", index=4, total=5)
    record.update(
        phase="updating", next={"release": release, "files": manifest["files"]}
    )
    saved(device, record)
    device.mkdir(release_dir(release))
    sent = upload(device, package, release, (record["release"], record["files"]))
    check_payload(device, record, release, manifest["files"])
    summary(f"{len(FILES)} files verified · {size(sent)} sent")
    step("Switch version", index=5, total=5)
    device.write(CURRENT, (release + "\n").encode())
    finish_update(device, record)
    summary(release)
    return Result(
        "updated",
        "Updated.",
        ("Reboot MIB manually to use the new version.",),
    )


def upload(device, package, release, installed=None):
    """Send each file; with `installed` (release, hashes), copy unchanged ones on the unit."""
    sent = 0
    for index, name in enumerate(FILES, 1):
        progress_item(name, index, len(FILES))
        path = release_file(release, name)
        attrs = payload_attrs(name)
        data = (package / name).read_bytes()
        if installed and installed[1][name] == sha256(data):
            device.copy(release_file(installed[0], name), path, attrs, sha=sha256(data))
            detail("Unchanged; copied on the unit and verified")
        else:
            device.write(path, data, attrs)
            sent += len(data)
            detail("Contents and permissions verified")
        item_done()
    return sent


def install(device, directory):
    step("Check MIB state")
    unfinished_initialization(
        device, directory.with_name(directory.name + "-initialization")
    )
    if device.exists(BASE):
        require(
            device.exists(JOURNAL),
            "Incomplete installation directory without a journal; preserve it for inspection",
        )
        record = settle_update(device, load(device))
        if record["phase"] == "installed":
            check_installed(device, record)
            summary("installed · unchanged")
            return update(device, directory, record)
        require(
            not record["may_run"],
            "Installation may have run; use remove to recover before reinstalling",
        )
        remove(device, directory.with_name(directory.name + "-rollback"))
    summary("ready to capture")
    step("Download originals", index=1, total=7)
    try:
        snapshot = capture(device, directory)
    except UnsupportedFirmware as error:
        step("Create stock archive")
        stock_context(device, directory)
        return stock_archive(directory, reason=str(error))
    device.preflight()
    require_workspace(directory)
    package = directory / "package"
    step("Build package", index=2, total=7)
    manifest = build_package(package, new_release())
    release = manifest["release"]
    summary(f"{len(FILES)} files verified")
    step("Recheck originals", index=3, total=7)
    unchanged(device, snapshot["files"])
    device.space(
        "/tsd/var",
        sum(p.stat().st_size for p in package.iterdir()) * 2 + 4 * 1024 * 1024,
    )
    device.space(
        "/", sum((package / n).stat().st_size for n in CONFIGS) * 2 + 1024 * 1024
    )
    summary("unchanged · space ok")
    record = {
        "schema": 3,
        "release": release,
        "profile": snapshot["profile"],
        "root_mode": device.root_mode(),
        "phase": "preparing",
        "may_run": False,
        "originals": snapshot["files"],
        "files": manifest["files"],
    }
    (directory / "install.json").write_bytes(json_bytes(record))
    step("Write journal & backups", index=4, total=7)
    device.mkdir(BASE)
    saved(device, record)
    device.mkdir(BASE + "/backup")
    device.mkdir(BASE + "/run")
    for name in CONFIGS:
        device.write(
            BASE + "/backup/" + name, (directory / "inputs/config" / name).read_bytes()
        )
        detail("Backup verified: " + name)
    summary(f"{len(CONFIGS)} configs backed up")
    step("Upload files", index=5, total=7)
    device.mkdir(RELEASES)
    device.mkdir(release_dir(release))
    uploaded = upload(device, package, release)
    check_payload(device, record)
    unchanged(device, snapshot["files"])
    summary(f"{len(FILES)} files · {size(uploaded)} verified")
    record["phase"] = "publishing"
    saved(device, record)
    step("Apply startup & firewall", index=6, total=7)
    with writable_root(device, record["root_mode"]):
        # Startup is published last; no mode exists during configuration writes.
        for name in ("pf.conf", "pf_wlan.conf", "startup"):
            original = record["originals"][name]
            unchanged(device, {name: original})
            device.write(
                REMOTE[name], (package / name).read_bytes(), original["attrs"], name
            )
            detail("Applied and verified: " + name)
    summary("pf.conf · pf_wlan.conf · startup")
    # Record that patched services may run (so removal knows Bluetooth UUID
    # recovery is needed) before `current` and `mode` let boot select them.
    step("Enable for next boot", index=7, total=7)
    record["may_run"] = True
    record["phase"] = "installed"
    saved(device, record)
    device.write(CURRENT, (release + "\n").encode())
    device.write(BASE + "/mode", b"normal\n")
    summary("journal: installed")
    return Result(
        "installed",
        "Installed.",
        ("Reboot MIB manually to enable Wireless Android Auto.",),
    )


def reboots(first_done):
    outline(
        "Removal needs two reboots.",
        (
            ("done" if first_done else "now", 1, "Schedule Bluetooth recovery", ""),
            ("done" if first_done else "next", 0, "reboot MIB", ""),
            (
                "now" if first_done else "next",
                2,
                "Restore configuration",
                "now" if first_done else "run Remove again",
            ),
            ("next", 0, "reboot MIB", ""),
        ),
    )


def remove(device, directory):
    step("Check installation")
    unfinished_initialization(device, directory)
    if not device.exists(BASE):
        summary("not installed")
        return Result("not_installed", "Wireless Android Auto is not installed.")
    record = settle_update(device, load(device))
    summary("journal: " + record["phase"])
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    (directory / "install.json").write_bytes(json_bytes(record))
    processes = device.processes()
    require(
        record["may_run"]
        or not running(processes, (*RUNNING_PATCHED, RUNNING_RECOVERY)),
        "Unexpected patched service; cannot prove rollback without UUID recovery",
    )
    require(
        not running(processes, RUNNING_PATCHED)
        or record["phase"] not in ("restoring", "cleaning"),
        "Normal patched services are still running; reboot first",
    )
    if record["phase"] == "cleaning":
        if record["may_run"]:
            reboots(first_done=True)
        step("Finish cleanup")
        for name in CONFIGS:
            require(
                sha256(device.read(REMOTE[name])) == record["restored"][name],
                "Restored configuration changed",
            )
        with writable_root(device, record["root_mode"]):
            for name in CONFIGS:
                device.unlink(REMOTE[name] + ".waa-new")
        device.cleanup()
        summary("restored configuration verified")
        return Result(
            "removed",
            "Removed.",
            ("Reboot MIB manually to use the original services.",),
        )

    recovery = record["may_run"] and record["phase"] != "restoring"
    if record["may_run"] and not recovery:
        reboots(first_done=True)
    if recovery:
        step("Check Bluetooth recovery")
        check_payload(device, record)
        recheck(device, record, LARGE)
        token = record.get("token")
        proof = BASE + "/run/restore-result"
        confirmed = (
            token
            and device.exists(proof)
            and device.attrs(proof) == PRIVATE
            and device.read(proof) == (token + "\n").encode()
        )
        if confirmed:
            summary("receipt matches token")
            reboots(first_done=True)
            processes = device.processes()
            require(
                not running(processes, RUNNING_PATCHED),
                "Normal patched services are still running; reboot first",
            )
            record["phase"] = "restoring"
            saved(device, record)
        else:
            summary("no receipt yet")
            reboots(first_done=False)
            step("Schedule BT recovery")
            require(
                device.read(REMOTE["startup"]).count(configuration.START_BLOCK) == 1,
                "Recovery startup block was changed or removed",
            )
            # Never replace the token/result under a running recovery process.
            require(
                RUNNING_RECOVERY not in device.processes(),
                "Recovery has no successful receipt yet; wait or reboot before retrying remove",
            )
            # Disarm before replacing attempt data. A crash here cannot select normal mode.
            device.write(BASE + "/mode", b"disabled\n")
            token = secrets.token_hex(16)
            record.update(phase="recovering", token=token)
            saved(device, record)
            device.write(BASE + "/run/restore-token", (token + "\n").encode())
            device.write(proof, b"")
            device.unlink(BASE + "/mode.boot")
            device.write(BASE + "/mode", ("restore:" + token + "\n").encode())
            summary("runs on next boot")
            return Result(
                "recovery_pending",
                "Recovery scheduled. Removal is not complete yet.",
                ("Reboot MIB, then run ./setup.sh and choose Remove again.",),
            )

    # Prepare every restoration before changing any system configuration.
    step("Prepare original configs")
    replacements = {}
    before = {}
    for name in CONFIGS:
        original = record["originals"][name]
        backup = BASE + "/backup/" + name
        current = device.read(REMOTE[name])
        before[name] = {"sha256": sha256(current), "attrs": device.attrs(REMOTE[name])}
        require(
            before[name]["attrs"] == original["attrs"],
            "Config attributes changed: " + name,
        )
        if "restored" in record and sha256(current) == record["restored"][name]:
            restored = current
        elif record["phase"] == "preparing":
            require(
                sha256(current) == original["sha256"],
                "Configuration changed before publication",
            )
            restored = current
        else:
            old = device.read(backup)
            require(sha256(old) == original["sha256"], "Backup damaged: " + name)
            restored = configuration.undo(name, current, old)
        replacements[name] = restored
        (directory / name).write_bytes(restored)
    summary(f"{len(CONFIGS)} configs ready")
    record.update(
        phase="restoring", restored={n: sha256(d) for n, d in replacements.items()}
    )
    saved(device, record)
    device.write(BASE + "/mode", b"disabled\n")
    step("Restore configuration")
    with writable_root(device, record["root_mode"]):
        for name, data in replacements.items():
            unchanged(device, {name: before[name]})
            device.write(REMOTE[name], data, record["originals"][name]["attrs"], name)
            device.unlink(REMOTE[name] + ".waa-new")
            detail("Restored and verified: " + name)
    summary("pf.conf · pf_wlan.conf · startup")
    record["phase"] = "cleaning"
    saved(device, record)
    step("Clean up WAA files")
    device.cleanup()
    summary(BASE + " removed")
    return Result(
        "removed", "Removed.", ("Reboot MIB manually to use the original services.",)
    )
