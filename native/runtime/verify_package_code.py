#!/usr/bin/env python3
"""Verify entire patched binaries against the private originals and current build."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from installer.elf import require, sha256
from installer.firmware import INPUTS, checked_build
from installer.patching import patch_image
from installer.paths import PATCHED, RECOVERY, workdir


def verify(package):
    manifest = json.loads((package / "manifest.json").read_text())
    binding = checked_build()
    require(
        manifest["firmware_profile"] == binding["profile"]
        and manifest["profile_sha256"] == binding["profile_sha256"],
        "Package profile differs",
    )
    for role, name in PATCHED.items():
        original = (workdir() / "inputs" / INPUTS[role]).read_bytes()
        blob = (workdir() / f"build/{role}_target.elf").read_bytes()
        actual = (package / name).read_bytes()
        require(
            sha256(actual) == manifest["files"][name], "Package hash differs: " + name
        )
        expected = patch_image(
            original,
            blob,
            binding[role],
            enabled=role == "wa",
        )
        require(actual == expected, "Undeclared binary change: " + name)
    original = (workdir() / "inputs" / INPUTS["wa"]).read_bytes()
    blob = (workdir() / "build/restore.elf").read_bytes()
    actual = (package / RECOVERY).read_bytes()
    require(
        actual == patch_image(original, blob, binding["wa"], "restore"),
        "Recovery differs",
    )
    require(
        sha256(actual) == manifest["files"][RECOVERY],
        "Recovery hash differs",
    )
    print(
        "PASS complete WA/SAL/recovery images: original bytes, all hooks and extension payloads"
    )


if __name__ == "__main__":
    verify(Path(sys.argv[1]))
