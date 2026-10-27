"""Temporary SHA-256 command using the unit's own QNX startup and loader."""

import gzip
import hashlib
import struct
import subprocess
import uuid
from contextlib import redirect_stdout

from installer.device import EXECUTABLE, quote
from installer.elf import (
    Image,
    append_segment,
    arm_bl_encode,
    is_sha256,
    load_header,
    require,
)
from installer.paths import ROOT, workdir
from installer.progress import detail
from native.runtime.linker import link


def prepare(device):
    directory = workdir() / "checksum"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = "/tsd/var/.waa-hash-" + uuid.uuid4().hex
    packed = temporary + ".gz"
    try:
        device.command("(umask 077; gzip -1 -c /bin/cat > " + quote(packed) + ")")
        raw = gzip.decompress(device.read(packed))
    finally:
        device.command("rm -f " + quote(packed))
    require(0 < len(raw) <= 128 * 1024, "Unsupported QNX cat size")
    stock = Image(raw, "ET_EXEC")
    header = "\n".join(
        "#define HASH_" + name + " " + hex(stock.symbol(symbol))
        for name, symbol in (
            ("OPEN", "open64"),
            ("READ", "read"),
            ("WRITE", "write"),
            ("CLOSE", "close"),
        )
    )
    (directory / "hash_generated.h").write_text(header + "\n")
    command = [
        "clang",
        "-target",
        "armv7-none-eabi",
        "-marm",
        "-mcpu=cortex-a9",
        "-ffreestanding",
        "-fno-builtin",
        "-nostdlib",
        "-fno-pic",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-fno-addrsig",
        "-O2",
        "-I",
        str(directory),
        "-c",
        str(ROOT / "native/hash/sha256.c"),
        "-o",
        str(directory / "hash.o"),
    ]
    detail("Preparing temporary on-device SHA-256 verification")
    with (directory / "build.log").open("w") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        with redirect_stdout(log):
            link(directory / "hash.o", stock.base, directory / "hash.elf", "hash")
    blob = Image((directory / "hash.elf").read_bytes(), "ET_EXEC")
    segment = blob.loads[0]
    data = bytearray(raw)
    main = stock.symbol("main")
    stock.unrelocated(main)
    struct.pack_into(
        "<I",
        data,
        stock.offset(main, 4, flags=1),
        arm_bl_encode(main, blob.elf["e_entry"]) & ~(1 << 24),
    )
    append = load_header(
        data,
        stock.note_header(),
        stock.base,
        segment["p_filesz"],
        segment["p_memsz"],
        5,
    )
    append_segment(data, append, segment.data())
    require(len(data) < 64 * 1024, "Unsupported SHA-256 helper size")
    try:
        device.write(temporary, bytes(data), EXECUTABLE)
        actual = device.command(quote(temporary) + " /bin/cat").decode("ascii").strip()
        require(
            actual == hashlib.sha256(raw).hexdigest(),
            "On-device SHA-256 self-test failed",
        )
    except BaseException:
        device.command(
            "rm -f " + quote(temporary) + " " + quote(temporary + ".waa-new")
        )
        raise
    return temporary


def digest(device, helper, path):
    data = device.command(quote(helper) + " " + quote(path)).decode("ascii").strip()
    require(is_sha256(data), "Invalid device SHA-256 result")
    return data
