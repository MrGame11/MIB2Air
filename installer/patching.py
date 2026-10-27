"""One checked patch engine for WA, SAL and UUID recovery. No device I/O."""

import os
import struct
import tempfile
from pathlib import Path

from installer.elf import (
    Image,
    append_segment,
    arm_bl_decode,
    arm_bl_encode,
    load_header,
    require,
    sha256,
)


def payload(blob, base):
    image = Image(blob, "ET_EXEC")
    require(len(image.loads) == 1, "Extension must contain exactly one LOAD")
    segment = image.loads[0]
    require(segment["p_vaddr"] == base, "Extension linked at wrong address")
    require(
        segment["p_filesz"] == segment["p_memsz"] > 0,
        "Extension contains BSS or is empty",
    )
    require(segment["p_flags"] == 7, "Expected RWX extension")
    require(not image.relocations(), "Unresolved extension relocations")
    symbols = {}
    for s in image.symbols(".symtab"):
        if s.name and s["st_shndx"] != "SHN_UNDEF":
            require(
                s.name not in symbols or symbols[s.name] == s["st_value"],
                "Ambiguous extension symbol: " + s.name,
            )
            symbols[s.name] = s["st_value"]
    return bytearray(segment.data()), symbols


def configured_payload(blob, base, enabled=False):
    data, symbols = payload(blob, base)
    if "waa_settings" in symbols:
        offset = symbols["waa_settings"] - base
        require(0 <= offset and offset + 4 <= len(data), "Settings outside extension")
        require(data[offset : offset + 4] == bytes(4), "Preconfigured extension")
        if enabled:
            data[offset : offset + 4] = struct.pack("<I", 0x57414131)
    else:
        require(not enabled, "No settings in extension")
    return data, symbols


def patch_image(original, blob, binding, mode="runtime", enabled=False):
    require(mode in ("runtime", "restore"), "Unknown patch mode")
    require(
        sha256(original) == binding["sha256"], "Original differs from resolved input"
    )
    stock = Image(original, "ET_EXEC")
    base = stock.base
    require(base == binding["base"], "Changed extension placement")
    addition, symbols = configured_payload(blob, base, enabled)
    require(base + len(addition) <= 0x100000000, "Extension address overflow")
    data = bytearray(original)
    changes = []
    occupied = set()
    for hook in binding["hooks"]:
        address, expected = hook["at"], hook["expected"]
        require(
            address % 4 == 0 and address not in occupied, "Overlapping/unaligned hooks"
        )
        occupied.add(address)
        kind = hook["kind"]
        require(kind in ("bl", "word", "keep"), "Unknown patch kind")
        offset = stock.offset(address, 4, flags=1 if kind == "bl" else 0)
        stock.unrelocated(address)
        actual = stock.word(address)
        if kind == "bl":
            actual = arm_bl_decode(actual, address)
        require(actual == expected, f"Unexpected hook target at {address:#x}")
        if mode not in hook:
            continue
        require(kind != "keep", "Preserved hook cannot be patched")
        name = hook[mode]
        require(name in symbols, "Missing extension symbol: " + name)
        target = symbols[name]
        require(
            base <= target < base + len(addition) and target % 4 == 0,
            "Invalid extension entry: " + name,
        )
        word = arm_bl_encode(address, target) if kind == "bl" else target
        struct.pack_into("<I", data, offset, word)
        changes.append((offset, offset + 4))
    require(bool(changes), "No hooks selected")
    header = stock.note_header()
    require(
        all(end <= header or start >= header + 32 for start, end in changes),
        "Hook overlaps reused program header",
    )
    append = load_header(data, header, base, len(addition), len(addition), 7)
    changes.append((header, header + 32))
    # Check complete untouched ranges, including every other program header and QNX note.
    end = 0
    for start, stop in sorted(changes):
        require(end <= start, "Overlapping modifications")
        require(data[end:start] == original[end:start], "Undeclared original change")
        end = stop
    require(data[end:] == original[end:], "Undeclared original change")
    append_segment(data, append, addition)
    result = Image(data, "ET_EXEC")
    segment = [s for s in result.loads if s["p_vaddr"] == base]
    require(
        len(segment) == 1 and segment[0].data() == addition, "Extension mapping differs"
    )
    for hook in binding["hooks"]:
        actual = result.word(hook["at"])
        if hook["kind"] == "bl":
            actual = arm_bl_decode(actual, hook["at"])
        expected = symbols[hook[mode]] if mode in hook else hook["expected"]
        require(actual == expected, "Final hook verification failed")
    return bytes(data)


def write_new(path, data, originals=(), replace=False):
    """Publish validated bytes; replacing private build outputs requires explicit opt-in."""
    path = Path(path)
    for original in originals:
        require(path.resolve() != Path(original).resolve(), "Output aliases an input")
        if path.exists():
            require(not path.samefile(original), "Output hardlinks an input")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".waa-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # A same-filesystem hard link publishes atomically without replacing anything.
        if replace:
            require(not path.is_symlink(), "Output is a symlink")
            os.replace(temporary, path)
        else:
            os.link(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
