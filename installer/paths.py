"""Fixed device paths and a private, per-installation build workspace."""

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = "/tsd/var/wireless-aa"
CURRENT = BASE + "/current"
RELEASES = BASE + "/releases"
RELEASE_ID = r"[0-9a-f]{16}"
REMOTE = {
    "wa": "/tsd/bin/wirelessadapter/tsd.mibstd2.system.wirelessadapter.main",
    "sal": "/tsd/bin/sal/tsd.mibstd2.sal.main",
    "libc": "/lib/libc.so.3",
    "gal": "/tsd/lib/sal/gal/libext.google.gal.receiver.so",
    "startup": "/tsd/bin/system/startup",
    "cpu.conf": "/tsd/etc/system/cpu.conf",
    "pf.conf": "/tsd/etc/networking/pf.conf",
    "pf_wlan.conf": "/tsd/etc/networking/pf_wlan.conf",
}
CONFIGS = ("startup", "pf.conf", "pf_wlan.conf")
# Patched service copies by role, and the Bluetooth UUID recovery image.
PATCHED = {"wa": "wirelessadapter.waa", "sal": "sal.waa"}
RECOVERY = "wirelessadapter.restore"


def release_dir(release):
    return RELEASES + "/" + release


def release_file(release, name):
    return release_dir(release) + "/" + name


def workdir():
    return Path(os.environ.get("WAA_WORKDIR", ROOT / "local")).resolve()


def json_bytes(value):
    import json

    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode()
