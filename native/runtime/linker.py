#!/usr/bin/env python3
"""Checked ARM object relocation and ELF emission with explicit WA/SAL/recovery policies."""

import struct
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from installer.elf import Refuse

R_ARM_ABS32, R_ARM_CALL, R_ARM_JUMP24 = 2, 28, 29
R_ARM_MOVW_ABS_NC, R_ARM_MOVT_ABS = 43, 44
KNOWN = {R_ARM_ABS32, R_ARM_CALL, R_ARM_JUMP24, R_ARM_MOVW_ABS_NC, R_ARM_MOVT_ABS}
PAGE = 0x1000

# Entry points the recovery image exports; WA/hash compute theirs, SAL exports all symbols.
EXPORTS = (
    "restore_result",
    "waa_ext_entry",
    "waa_ext_register",
    "waa_rx_oneshot_entry",
    "waa_rx_oneshot_c",
    "waa_uuid_result_cb",
    "waa_ext_send_uuid_set",
    "waa_ext_restore_uuid",
    "g_waa_oneshot",
)


def alloc_sections(elf, role):
    SHF_ALLOC = 0x2
    out = []
    for s in elf.iter_sections():
        if (
            s["sh_type"] in ("SHT_PROGBITS", "SHT_NOBITS")
            and (s["sh_flags"] & SHF_ALLOC)
            and s.name != ".ARM.exidx"
        ):
            out.append(s)
    # deterministic: executable .text first, then rodata by name
    out.sort(
        key=lambda s: (
            (s["sh_type"] == "SHT_NOBITS") if role == "sal" else False,
            0 if (s["sh_flags"] & 0x4) else 1,
            s.name,
        )
    )
    return out


def link(inp, base, outp, role):
    if role not in ("wa", "sal", "restore", "hash"):
        raise ValueError("Unknown link role")
    with open(inp, "rb") as f:
        elf = ELFFile(f)
        if elf["e_machine"] not in ("EM_ARM", 40):
            raise Refuse("Linker input is not ARM")
        if elf["e_type"] != "ET_REL":
            raise Refuse("Linker input must be an ET_REL object")
        secs = alloc_sections(elf, role)
        # layout
        img = bytearray()
        sec_base = {}  # section name -> vaddr
        for s in secs:
            al = max(1, s["sh_addralign"])
            while (base + len(img)) % al:
                img.append(0)
            sec_base[s.name] = base + len(img)
            img += bytes(s["sh_size"]) if s["sh_type"] == "SHT_NOBITS" else s.data()
        secidx_base = {}
        for i, s in enumerate(elf.iter_sections()):
            if s.name in sec_base:
                secidx_base[i] = sec_base[s.name]

        symtab = elf.get_section_by_name(".symtab")

        def sym_vaddr(si):
            sym = symtab.get_symbol(si)
            shndx = sym["st_shndx"]
            if shndx in secidx_base:
                return secidx_base[shndx] + sym["st_value"]
            raise Refuse(f"Unresolved symbol index {si} ({sym.name!r}) shndx={shndx}")

        def patch_at(vaddr, newbytes):
            off = vaddr - base
            img[off : off + len(newbytes)] = newbytes

        def rd32(vaddr):
            off = vaddr - base
            return int.from_bytes(img[off : off + 4], "little")

        for rs in elf.iter_sections():
            if not isinstance(rs, RelocationSection):
                continue
            target = elf.get_section(rs["sh_info"])
            if target.name not in sec_base:
                continue  # relocations for dropped sections (e.g. .ARM.exidx)
            tbase = sec_base[target.name]
            for r in rs.iter_relocations():
                rt = r["r_info_type"]
                if rt not in KNOWN:
                    raise Refuse(f"Unknown relocation type {rt} in {rs.name}")
                P = tbase + r["r_offset"]
                S = sym_vaddr(r["r_info_sym"])
                w = rd32(P)
                if rt == R_ARM_ABS32:
                    A = w
                    patch_at(P, ((S + A) & 0xFFFFFFFF).to_bytes(4, "little"))
                elif rt in (R_ARM_CALL, R_ARM_JUMP24):
                    A = w & 0x00FFFFFF
                    if A & 0x00800000:
                        A -= 0x01000000
                    A <<= 2
                    off = (S + A - P) >> 2
                    if off < -0x800000 or off > 0x7FFFFF:
                        raise Refuse("ARM CALL relocation out of range")
                    patch_at(
                        P, ((w & 0xFF000000) | (off & 0x00FFFFFF)).to_bytes(4, "little")
                    )
                elif rt in (R_ARM_MOVW_ABS_NC, R_ARM_MOVT_ABS):
                    # imm16 split: [19:16]=imm4, [11:0]=imm12
                    A = ((w >> 4) & 0xF000) | (w & 0x0FFF)
                    val = (S + A) & 0xFFFFFFFF
                    imm16 = (
                        val & 0xFFFF
                        if rt == R_ARM_MOVW_ABS_NC
                        else (val >> 16) & 0xFFFF
                    )
                    w2 = (w & 0xFFF0F000) | ((imm16 & 0xF000) << 4) | (imm16 & 0x0FFF)
                    patch_at(P, w2.to_bytes(4, "little"))

        # a writable alloc section (e.g. .data for the one-shot flag) makes the blob RWX
        writable = any((s["sh_flags"] & 0x1) for s in secs)  # SHF_WRITE

        exports = EXPORTS
        if role in ("wa", "hash"):
            exports = tuple(
                dict.fromkeys(
                    symbol.name
                    for symbol in symtab.iter_symbols()
                    if symbol.name
                    and not symbol.name.startswith("$")
                    and isinstance(symbol["st_shndx"], int)
                )
            )
        symmap = {}
        for symbol in symtab.iter_symbols():
            selected = (
                bool(symbol.name)
                and symbol["st_info"]["type"] in ("STT_FUNC", "STT_OBJECT")
                if role == "sal"
                else symbol.name in exports
            )
            if selected and symbol["st_shndx"] in secidx_base:
                symmap[symbol.name] = (
                    secidx_base[symbol["st_shndx"]] + symbol["st_value"]
                )
        entry_name = {
            "wa": "waa_wa_entry",
            "sal": "waa_tcp_ctor_hook",
            "restore": "waa_ext_entry",
            "hash": "waa_hash_main",
        }[role]
        if entry_name not in symmap:
            raise ValueError("Missing entry: " + entry_name)
        entry = symmap[entry_name]
        if role == "sal":
            entry &= ~1
            writable = True
            elf_symbols = sorted(symmap.items(), key=lambda item: item[1])
        else:
            elf_symbols = [(name, symmap[name]) for name in exports if name in symmap]

    write_elf(outp, base, bytes(img), entry, elf_symbols=elf_symbols, writable=writable)
    print(
        f"linked {outp}: BASE=0x{base:08x} size={len(img)} entry=0x{entry:08x} "
        f"perms={'RWX' if writable else 'R-X'}"
    )


def write_elf(path, base, image, entry, elf_symbols, writable=False):
    # file: [ehdr 52][phdr 32][pad to 0x1000][image][strtab][symtab][shstrtab][shdrs]
    IMG_OFF = PAGE
    data = bytearray()
    # placeholder ehdr+phdr, fill later
    data += b"\x00" * 52
    data += b"\x00" * 32
    data += b"\x00" * (IMG_OFF - len(data))
    img_off = len(data)
    data += image
    # .strtab
    strtab = b"\x00"
    names = {}
    for n, _ in elf_symbols:
        names[n] = len(strtab)
        strtab += n.encode() + b"\x00"
    strtab_off = len(data)
    data += strtab
    # .symtab (index 0 = null; then our syms as STT_FUNC in section 1 = .blob)
    symtab = b"\x00" * 16
    STT_FUNC, GLOBAL = 2, 1
    for n, v in elf_symbols:
        info = (GLOBAL << 4) | STT_FUNC
        symtab += struct.pack("<IIIBBH", names[n], v, 0, info, 0, 1)
    symtab_off = len(data)
    data += symtab
    # .shstrtab
    shstr = b"\x00"

    def addname(s):
        nonlocal shstr
        o = len(shstr)
        shstr += s.encode() + b"\x00"
        return o

    n_blob = addname(".blob")
    n_symtab = addname(".symtab")
    n_strtab = addname(".strtab")
    n_shstr = addname(".shstrtab")
    shstr_off = len(data)
    data += shstr
    # section headers
    while len(data) % 4:
        data += b"\x00"
    shoff = len(data)
    SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB = 1, 2, 3
    SHF_ALLOC, SHF_EXEC, SHF_WRITE = 0x2, 0x4, 0x1
    blob_flags = SHF_ALLOC | SHF_EXEC | (SHF_WRITE if writable else 0)

    def shdr(name, typ, flags, addr, off, size, link=0, info=0, align=1, entsize=0):
        return struct.pack(
            "<10I", name, typ, flags, addr, off, size, link, info, align, entsize
        )

    sh = b""
    sh += shdr(0, 0, 0, 0, 0, 0)  # null
    sh += shdr(
        n_blob, SHT_PROGBITS, blob_flags, base, img_off, len(image), align=PAGE
    )  # 1 .blob
    sh += shdr(
        n_symtab,
        SHT_SYMTAB,
        0,
        0,
        symtab_off,
        len(symtab),
        link=3,
        info=1,
        align=4,
        entsize=16,
    )  # 2
    sh += shdr(n_strtab, SHT_STRTAB, 0, 0, strtab_off, len(strtab), align=1)  # 3
    sh += shdr(n_shstr, SHT_STRTAB, 0, 0, shstr_off, len(shstr), align=1)  # 4
    data += sh
    shnum, shstrndx = 5, 4

    # ELF header
    e_ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr = e_ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2,  # ET_EXEC
        40,  # EM_ARM
        1,  # version
        entry,  # e_entry
        52,  # e_phoff
        shoff,  # e_shoff
        0x05000000,  # e_flags (EABI5)
        52,
        32,
        1,  # ehsize, phentsize, phnum
        40,
        shnum,
        shstrndx,
    )
    data[0:52] = ehdr
    # program header: PT_LOAD R-X, or RWX when a writable section is present
    PT_LOAD, PF_R, PF_W, PF_X = 1, 4, 2, 1
    pflags = PF_R | PF_X | (PF_W if writable else 0)
    phdr = struct.pack(
        "<8I", PT_LOAD, img_off, base, base, len(image), len(image), pflags, PAGE
    )
    data[52 : 52 + 32] = phdr

    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    role, source, base, output = sys.argv[1:]
    link(source, int(base, 0), output, role)
