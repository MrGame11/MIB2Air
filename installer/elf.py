"""Checked ARM ELF operations shared by resolution, patching and verification."""

import hashlib
import io
import re
import struct

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


class Refuse(ValueError):
    """The input does not satisfy a required compatibility condition."""


def require(condition, message):
    if not condition:
        raise Refuse(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_header(data, header, base, filesz, memsz, flags):
    """Rewrite the program header at `header` as a LOAD for a segment appended at the
    next page boundary; returns that file offset."""
    append = (len(data) + 4095) & ~4095
    struct.pack_into(
        "<8I", data, header, 1, append, base, base, filesz, memsz, flags, 4096
    )
    return append


def append_segment(data, append, payload):
    data.extend(bytes(append - len(data)))
    data.extend(payload)


def is_sha256(value):
    return type(value) is str and re.fullmatch("[0-9a-f]{64}", value) is not None


def arm_bl_decode(word, pc):
    require(word & 0xFF000000 == 0xEB000000, f"Not an unconditional ARM BL at {pc:#x}")
    displacement = word & 0xFFFFFF
    if displacement & 0x800000:
        displacement -= 0x1000000
    return pc + 8 + displacement * 4


def arm_bl_encode(pc, target):
    delta = target - pc - 8
    require(pc % 4 == target % 4 == 0, "Unaligned ARM branch")
    require(-0x2000000 <= delta < 0x2000000, "ARM BL out of range")
    return 0xEB000000 | ((delta // 4) & 0xFFFFFF)


def arm_immediate(word):
    shift = ((word >> 8) & 15) * 2
    value = word & 255
    return ((value >> shift) | (value << (32 - shift))) & 0xFFFFFFFF if shift else value


class Image:
    def __init__(self, data, kind=None):
        self.data = bytes(data)
        self._symbol_tables = {}
        self._relocation_records = None
        try:
            self.elf = ELFFile(io.BytesIO(self.data))
        except Exception as exc:
            raise Refuse("Invalid ELF") from exc
        e = self.elf
        require(e.elfclass == 32 and e.little_endian, "Expected little-endian ELF32")
        require(e["e_machine"] == "EM_ARM", "Expected ARM ELF")
        require(e["e_flags"] >> 24 == 5, "Expected ARM EABI5")
        require(e["e_type"] in ("ET_EXEC", "ET_DYN"), "Expected linked ELF")
        require(kind is None or e["e_type"] == kind, f"Expected {kind}")
        require(e["e_phentsize"] == 32, "Unexpected program header size")
        require(
            e["e_phoff"] + e["e_phnum"] * 32 <= len(data), "Truncated program headers"
        )
        self.loads = [s for s in e.iter_segments() if s["p_type"] == "PT_LOAD"]
        require(bool(self.loads), "No loadable segments")
        for seg in self.loads:
            require(seg["p_filesz"] <= seg["p_memsz"], "Invalid segment size")
            require(seg["p_offset"] + seg["p_filesz"] <= len(data), "Truncated segment")
            require(seg["p_vaddr"] + seg["p_memsz"] <= 0x100000000, "Segment overflow")

    @property
    def base(self):
        end = max(s["p_vaddr"] + s["p_memsz"] for s in self.loads)
        return (end + 4095) & ~4095

    def offset(self, address, size=4, flags=0):
        matches = [
            s
            for s in self.loads
            if s["p_vaddr"] <= address
            and address + size <= s["p_vaddr"] + s["p_filesz"]
            and s["p_flags"] & flags == flags
        ]
        require(
            size > 0 and len(matches) == 1, f"Unmapped/ambiguous address {address:#x}"
        )
        s = matches[0]
        return s["p_offset"] + address - s["p_vaddr"]

    def read(self, address, size, flags=0):
        off = self.offset(address, size, flags)
        return self.data[off : off + size]

    def mapped(self, address, size=4):
        require(
            sum(
                s["p_vaddr"] <= address
                and address + size <= s["p_vaddr"] + s["p_memsz"]
                for s in self.loads
            )
            == 1,
            f"Unmapped runtime address {address:#x}",
        )

    def word(self, address):
        return struct.unpack("<I", self.read(address, 4))[0]

    def symbols(self, table=".dynsym"):
        if table in self._symbol_tables:
            return self._symbol_tables[table]
        section = self.elf.get_section_by_name(table)
        if section is None and table == ".dynsym":
            dynamic = [
                s for s in self.elf.iter_segments() if s["p_type"] == "PT_DYNAMIC"
            ]
            require(len(dynamic) == 1, "Missing dynamic symbol table")
            section = dynamic[0]
        require(section is not None, f"Missing {table}")
        self._symbol_tables[table] = list(section.iter_symbols())
        return self._symbol_tables[table]

    def symbol(self, name, table=".dynsym"):
        found = {
            s["st_value"]
            for s in self.symbols(table)
            if s.name == name and s["st_value"]
        }
        require(len(found) == 1, f"Missing/ambiguous symbol {name}")
        return found.pop()

    def relocations(self):
        """(relocation, symbol name) pairs, read once."""
        if self._relocation_records is not None:
            return self._relocation_records
        records = []
        for section in self.elf.iter_sections():
            if isinstance(section, RelocationSection):
                symbols = self.elf.get_section(section["sh_link"])
                for rel in section.iter_relocations():
                    records.append((rel, symbols.get_symbol(rel["r_info_sym"]).name))
        self._relocation_records = records
        return records

    def got(self, name):
        slots = [
            r["r_offset"]
            for r, n in self.relocations()
            if n == name and r["r_info_type"] == 22
        ]
        require(len(slots) == 1, f"Missing/ambiguous JUMP_SLOT for {name}")
        self.offset(slots[0], 4, flags=2)
        return slots[0]

    def function(self, name):
        """Resolve and validate a definition or its canonical ARM import stub."""
        address = self.symbol(name)
        require(address % 4 == 0, f"Non-ARM function {name}")
        self.offset(address, 4, flags=1)
        imported = any(
            s.name == name and s["st_shndx"] == "SHN_UNDEF" for s in self.symbols()
        )
        if imported:
            self.plt(name, address)
        return address

    def plt(self, name, address):
        """Validate an import stub even when an ET_DYN symbol has st_value=0."""
        require(address % 4 == 0, "Unaligned PLT")
        a, b, c = struct.unpack("<3I", self.read(address, 12, flags=1))
        require(
            a & 0xFFFFF000 == 0xE28FC000
            and b & 0xFFFFF000 == 0xE28CC000
            and c & 0xFFFFF000 == 0xE5BCF000,
            f"Unsupported PLT for {name}",
        )
        slot = address + 8 + arm_immediate(a) + arm_immediate(b) + (c & 4095)
        require(slot == self.got(name), f"PLT/GOT mismatch for {name}")
        return address

    def note_header(self):
        notes = [
            (i, [n["n_name"] for n in s.iter_notes()])
            for i, s in enumerate(self.elf.iter_segments())
            if s["p_type"] == "PT_NOTE"
        ]
        matches = [i for i, owners in notes if owners == ["GNU"]]
        require(len(matches) == 1, "Expected exactly one reusable GNU note")
        require(any(owners == ["QNX"] for _, owners in notes), "Missing QNX note")
        return self.elf["e_phoff"] + matches[0] * 32

    def unrelocated(self, address, size=4):
        for r, _ in self.relocations():
            require(
                not (r["r_offset"] < address + size and address < r["r_offset"] + 4),
                f"Relocation overlaps patch at {address:#x}",
            )
