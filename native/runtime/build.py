#!/usr/bin/env python3
"""Compile and link private WA, SAL and recovery extensions."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from installer.firmware import (
    INPUTS,
    checked_binding,
    finish_build,
    resolve,
    write_headers,
)
from installer.patching import patch_image, write_new
from installer.paths import workdir
from native.runtime.linker import link


def build():
    output = workdir() / "build"
    output.mkdir(parents=True, exist_ok=True)
    (output / "build-manifest.json").unlink(missing_ok=True)
    binding = resolve()
    write_headers(binding, output)
    print("Resolved profile:", binding["profile"], flush=True)
    flags = [
        "clang",
        "-target",
        "armv7-none-eabi",
        "-marm",
        "-mcpu=cortex-a9",
        "-ffreestanding",
        "-nostdlib",
        "-fno-pic",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-fno-addrsig",
        "-fno-vectorize",
        "-fno-slp-vectorize",
        "-O2",
        "-Xclang",
        "-target-feature",
        "-Xclang",
        "-neon",
        "-I",
        str(output),
    ]
    for role in ("wa", "sal", "restore"):
        stem = role + "_target" if role != "restore" else "restore"
        extra = (
            ["-fno-exceptions", "-fno-stack-protector", "-DWAA_MODE=2", "-DWAA_ROLE_WA"]
            if role == "restore"
            else ["-ffunction-sections"]
        )
        obj, elf = output / (stem + ".o"), output / (stem + ".elf")
        subprocess.run(
            flags
            + extra
            + ["-c", str(ROOT / f"native/runtime/{role}_target.c"), "-o", str(obj)],
            check=True,
        )
        binding = checked_binding()
        spec = binding["wa" if role == "restore" else role]
        link(obj, spec["base"], elf, role)
        if role == "restore":
            source = workdir() / "inputs" / INPUTS["wa"]
            write_new(
                output / "wirelessadapter.restore",
                patch_image(source.read_bytes(), elf.read_bytes(), spec, "restore"),
                (source, elf),
                replace=True,
            )
    finish_build()


if __name__ == "__main__":
    build()
