#!/usr/bin/env python3
"""Data alignment: RISC OS 5 on ARMv7 normally runs with alignment
exceptions on, so an LDR/STR of a word at an address that isn't a
multiple of 4 aborts ("abort on data transfer"). RISC OS on Linux doesn't
check, so a program can work there and abort on a Pi.

    checks/check-data-align.py PROGRAM.elf
        A linked program: every data object of 4 bytes or more must be on a
        word boundary, and 2-byte ones on a halfword. Exit 1, listing them.
    checks/check-data-align.py LIB.a|OBJECT.o ...
        Libraries and objects: no writable data section (.data, .bss) of 4
        bytes or more may have an alignment below 4, because then where the
        linker puts it depends on what comes before it. Exit 1, listing them.
    checks/check-data-align.py --fix IN OUT
        Copy a library or object, raising those sections' alignment to 4
        (the linker then pads before them; nothing else changes).

Why: four of UnixLib 5.0.3.9's assembler files (and crt0.o) gave their
.data no alignment, and in Groove 0.2 to 0.4.2 UnixLib's start-up read a
word at an address ending in 6 and aborted on a Pi (Geofrey Baxendale's
report, 2026-11-21). Based on Groove's tools/check-align.py and
tools/align-libs.py (by the riscos-crossdev owner, used with permission).

Copyright (c) 2026 Andrew Youll. MIT licence.
"""
import struct
import sys

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS = 1, 2, 8
SHF_WRITE, SHF_ALLOC = 1, 2
STT_OBJECT = 1
ET_REL, ET_EXEC, ET_DYN = 1, 2, 3


def is_elf32le(b):
    return b[:4] == b"\x7fELF" and b[4] == 1 and b[5] == 1


def sections(b):
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", b, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4] if shnum else 0
    def name(s):
        return b[stroff + s[0]:b.index(b"\0", stroff + s[0])].decode()
    return shoff, shentsize, secs, name


def object_sections(b, label, fix):
    """Under-aligned writable data sections of one relocatable object:
    a list of messages, and (if fix) b changed in place."""
    if not is_elf32le(b):
        return []
    shoff, shentsize, secs, name = sections(b)
    out = []
    for i, s in enumerate(secs):
        _, typ, flags, _, _, size, _, _, align, _ = s
        if typ not in (SHT_PROGBITS, SHT_NOBITS) or (flags & (SHF_ALLOC | SHF_WRITE)) != (SHF_ALLOC | SHF_WRITE):
            continue
        if size < 4 or align >= 4:
            continue
        out.append("%s %s: %d bytes, aligned to %d" % (label, name(s), size, align))
        if fix:
            struct.pack_into("<I", b, shoff + i * shentsize + 32, 4)
    return out


def library(path, fix_to=None):
    """Check (or fix into fix_to) an ar archive or a single object."""
    a = bytearray(open(path, "rb").read())
    msgs = []
    if a[:8] == b"!<arch>\n":
        out = bytearray(a[:8])
        pos, longnames = 8, b""
        while pos < len(a):
            hdr = a[pos:pos + 60]
            name = hdr[:16].decode().strip()
            size = int(hdr[48:58].decode().strip())
            body = bytearray(a[pos + 60:pos + 60 + size])
            if name == "//":
                longnames = bytes(body)
            elif name.startswith("/") and name[1:].isdigit():
                o = int(name[1:])
                name = longnames[o:longnames.index(b"\n", o)].decode()
            if not name.startswith("/"):
                msgs += object_sections(body, "%s(%s)" % (path, name.rstrip("/")), fix_to is not None)
            out += hdr + body
            pos += 60 + size
            if size % 2:
                out += a[pos:pos + 1]
                pos += 1
    elif is_elf32le(a):
        msgs = object_sections(a, path, fix_to is not None)
        out = a
    else:
        sys.exit("%s: not an ar archive or an ELF32 object" % path)
    if fix_to is not None:
        open(fix_to, "wb").write(out)
    return msgs


def program(path):
    b = open(path, "rb").read()
    _, _, secs, _ = sections(b)
    bad = []
    for s in secs:
        if s[1] != SHT_SYMTAB:
            continue
        strtab = secs[s[6]]
        for o in range(s[4], s[4] + s[5], 16):
            nm, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", b, o)
            if info & 15 != STT_OBJECT or size < 2 or shndx == 0:
                continue
            need = 4 if size >= 4 else 2
            if value % need:
                n = b[strtab[4] + nm:b.index(b"\0", strtab[4] + nm)].decode()
                bad.append("%08x %d bytes %s" % (value, size, n))
    return bad


def main(argv):
    if not argv:
        sys.exit(__doc__)
    if argv[0] == "--fix":
        if len(argv) != 3:
            sys.exit("usage: check-data-align.py --fix IN OUT")
        for m in library(argv[1], argv[2]):
            print("fixed: " + m)
        return 0
    rc = 0
    for path in argv:
        b = open(path, "rb").read(64)
        if is_elf32le(b) and struct.unpack_from("<H", b, 0x10)[0] in (ET_EXEC, ET_DYN):
            bad = program(path)
            for x in bad:
                print("WRONG: misaligned data in %s (aborts on a Pi with alignment checking on): %s" % (path, x))
            if not bad:
                print("ok: %s: data aligned" % path)
        else:
            bad = library(path)
            for x in bad:
                print("WRONG: under-aligned data section (where it lands depends on the link): " + x)
            if not bad:
                print("ok: %s: data sections aligned" % path)
        rc |= 1 if bad else 0
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
