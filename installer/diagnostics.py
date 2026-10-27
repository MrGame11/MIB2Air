"""Read-only, best-effort diagnostic snapshots; all results stay on the host."""

import datetime
import re

from installer.ap_profile import AP_PATH, profile_from_xml
from installer.device import quote
from installer.elf import require, sha256
from installer.firmware import source_digest
from installer.paths import (
    BASE,
    CURRENT,
    PATCHED,
    RECOVERY,
    RELEASE_ID,
    RELEASES,
    REMOTE,
    json_bytes,
    release_file,
)
from installer.progress import detail, item, item_done, size, step, summary
from installer.result import Result

COMMANDS = {
    "clock": "date",
    "system": "uname -a",
    "uptime": "uptime",
    "processes": "pidin ar",
    "threads": "pidin threads",
    "memory": "pidin mem",
    "descriptors": "pidin fd",
    "process-timing": "pidin times",
    "interfaces": "ifconfig -a",
    "connections": "netstat -an",
    "routes": "netstat -rn",
    "interface-counters": "netstat -in",
    "arp": "arp -an",
    "disk": "df -kP",
    "mounts": "mount",
    "firewall-status": "pfctl -s info",
    "firewall-rules": "pfctl -sr",
    "firewall-anchors": "pfctl -a port_rules -sr",
    "firewall-states": "pfctl -ss",
    "root-flags": "df -g /",
    "native-wlan-mode": "cat /tsd/var/networking/wlanlastmode.conf",
    "boot-guard": "ls -ln /dev/shmem/wireless-aa.boot",
    "startup": "cat /tsd/bin/system/startup",
    "stock-cpu-config": "cat /tsd/etc/system/cpu.conf",
    "firewall-config": "cat /tsd/etc/networking/pf.conf /tsd/etc/networking/pf_wlan.conf /tsd/var/networking/port_rules.conf",
    "network-utilities": "ls /sbin /usr/sbin /bin /usr/bin",
    "crash-files": "ls -l /tsd/var/core /tsd/var/cores /var/dumps /dev/shmem",
    "service-events": "sloginfo -t | grep -E 'WAA_|AA_BOOT|GalManagerImpl|ReceiverLib|WIFI_APPL_NME_AP_STATION_IND|UG:'",
    "installation-files": "ls -lanR " + BASE,
}
STATE_FILES = (
    "install.json",
    "mode",
    "mode.boot",
    "run/wa.permit",
    "run/sal.permit",
    "run/restore-token",
    "run/restore-result",
    "current",
)
# Read from the active release named by `current`.
RELEASE_FILES = ("cpu.conf", "run.sh", "select.sh")


def cat_regular(path):
    """Read a regular file only; links and missing files fail the command."""
    q = quote(path)
    return "test -f " + q + " && test ! -L " + q + " && cat " + q


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def collect(device, directory):
    directory.mkdir(parents=True, exist_ok=False, mode=0o700)
    manifest = {
        "schema": 1,
        "collector_source_sha256": source_digest(),
        "started": utc(),
        "status": "collecting",
        "items": [],
        "limitations": "Phone-side state and packet payloads are not captured. Missing utilities are recorded with stderr and exit status; they do not abort the bundle.",
    }
    broken = False
    total_items = 1 + len(COMMANDS) + len(STATE_FILES) + len(RELEASE_FILES) + 2

    def save_manifest():
        (directory / "manifest.json").write_bytes(json_bytes(manifest))

    def capture(name, command, transform=None, reader=None):
        nonlocal broken
        record = {"name": "snapshot/" + name, "command": command, "started": utc()}
        item(name, len(manifest["items"]) + 1, total_items)
        try:
            raw, exit_code = reader() if reader else device.capture_command(command)
            record["exit_code"] = exit_code
            data = transform(raw) if transform and exit_code == 0 else raw
            path = directory / "snapshot" / name
            path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            path.write_bytes(data)
            path.chmod(0o600)
            record.update(
                status="ok" if exit_code == 0 else "unavailable",
                bytes=len(data),
                sha256=sha256(data),
            )
        except (OSError, RuntimeError) as error:
            # After a timeout/transport failure the shell stream may be unsynchronized.
            broken = True
            record.update(
                status="error", error=type(error).__name__ + ": " + str(error)
            )
        except ValueError as error:
            record.update(
                status="unavailable", error=type(error).__name__ + ": " + str(error)
            )
        except BaseException:
            record["status"] = "interrupted"
            raise
        finally:
            record["finished"] = utc()
            manifest["items"].append(record)
            save_manifest()
            item_done(record["status"])

    def ap_summary(raw):
        cfg = profile_from_xml(raw)
        return json_bytes(
            {
                "valid": True,
                "security": cfg["security"],
                "ap_type": cfg["ap_type"],
                "ssid_sha256": sha256(cfg["ssid"].encode()),
                "credentials_omitted": True,
            }
        )

    def executable_hashes():
        paths = {name: REMOTE[name] for name in ("wa", "sal", "libc", "gal")}
        result = {}
        failed = False
        try:
            raw, status = device.capture_command(cat_regular(CURRENT))
            release = raw.decode("ascii", "replace").strip()
            require(
                status == 0 and re.fullmatch(RELEASE_ID, release) is not None,
                "No valid active release",
            )
            paths.update(
                {
                    name: release_file(release, name)
                    for name in (*PATCHED.values(), RECOVERY)
                }
            )
        except ValueError as error:
            result["release"] = {"path": CURRENT, "unavailable": str(error)}
            failed = True
        for name, path in paths.items():
            try:
                result[name] = {"path": path, "sha256": device.sha256(path)}
            except ValueError as error:
                result[name] = {"path": path, "unavailable": str(error)}
                failed = True
        return json_bytes(result), int(failed)

    def snapshot():
        capture("system-log.txt", "sloginfo -t")
        for name, command in COMMANDS.items():
            if broken:
                break
            capture(name + ".txt", command)
        for relative in STATE_FILES:
            if broken:
                break
            capture("state/" + relative + ".txt", cat_regular(BASE + "/" + relative))
        for name in RELEASE_FILES:
            if broken:
                break
            capture(
                "state/release/" + name + ".txt",
                # A subshell keeps failures away from the interactive session.
                "(r=$(cat " + CURRENT + ") && case \"$r\" in *[!0-9a-f]*|'') false ;; "
                "*) f="
                + RELEASES
                + "/$r/"
                + name
                + ' && test -f "$f" && test ! -L "$f" && cat "$f" ;; esac)',
            )
        if not broken:
            capture(
                "executable-hashes.json",
                "SHA-256 of stock and installed executables",
                reader=executable_hashes,
            )
        if not broken:
            capture("ap-summary.json", cat_regular(AP_PATH), ap_summary)

    save_manifest()
    try:
        step("Capture snapshot")
        snapshot()
        if broken:
            detail("Connection lost")
        saved = [i for i in manifest["items"] if i["status"] == "ok"]
        summary(
            f"{len(saved)}/{len(manifest['items'])} items · "
            + size(sum(i.get("bytes", 0) for i in manifest["items"]))
            + (" · connection lost" if broken else "")
        )
        manifest["status"] = (
            "partial"
            if any(i["status"] != "ok" for i in manifest["items"])
            else "complete"
        )
    except BaseException:
        manifest["status"] = "interrupted"
        raise
    finally:
        manifest["finished"] = utc()
        save_manifest()
    partial = manifest["status"] == "partial"
    return Result(
        "diagnostics_partial" if partial else "diagnostics_complete",
        "Diagnostics saved" + (" (some data unavailable)." if partial else "."),
    )
