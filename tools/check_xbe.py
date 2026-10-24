"""Cross-checks the C++ loader's tables against a real default.xbe, without building anything.

  python -I tools/check_xbe.py <default.xbe>

- every kernel import of the XBE must exist in src/core/kernel_exports.cpp (and the table must be contiguous);
- runs the same fs: pattern scan as src/pc/fs_patch.cpp and prints what would be patched.
"""
import collections
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
KPCR_SIZE = 0x300
CONTEXT_SLOT = 0x14


def load_kernel_table():
    text = (ROOT / "src/core/kernel_exports.cpp").read_text()
    table = {}
    for kind, ordinal, name in re.findall(r"\b([FD])\((\d+), (\w+)\)", text):
        ordinal = int(ordinal)
        if ordinal in table:
            sys.exit(f"duplicate ordinal {ordinal}")
        table[ordinal] = (kind, name)
    missing = [o for o in range(1, max(table) + 1) if o not in table]
    if missing:
        sys.exit(f"kernel table has holes at {missing}")
    return table


def parse_xbe(data):
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    base = u32(0x104)
    sections = []
    for i in range(u32(0x11C)):
        o = u32(0x120) - base + i * 56
        flags, va, vsize, raw, rsize, name_addr = struct.unpack_from("<IIIIII", data, o)
        name = data[name_addr - base: data.index(b"\0", name_addr - base)].decode()
        sections.append((name, flags, va, vsize, raw, rsize))
    entry = u32(0x128) ^ 0xA8FC57AB
    retail = base <= entry < base + u32(0x10C)
    thunk = u32(0x158) ^ (0x5B6D40B6 if retail else 0xEFB1F152)
    return base, sections, thunk


def file_offset(sections, va):
    for name, flags, sva, vsize, raw, rsize in sections:
        if sva <= va < sva + rsize:
            return raw + va - sva
    raise ValueError(hex(va))


def decode(code, i):
    """Same patterns as decode() in src/pc/fs_patch.cpp: (length, kind, reg, offset) or None."""
    if code[i] != 0x64 or i + 6 > len(code):
        return None
    if code[i + 1] == 0xA1:
        return 6, "mov", 0, struct.unpack_from("<I", code, i + 2)[0]
    if i + 7 <= len(code) and code[i + 1] == 0x8B and (code[i + 2] & 0xC7) == 0x05:
        return 7, "mov", (code[i + 2] >> 3) & 7, struct.unpack_from("<I", code, i + 3)[0]
    if i + 8 <= len(code) and code[i + 1] == 0x0F and code[i + 2] == 0xB6 and (code[i + 3] & 0xC7) == 0x05:
        return 8, "movzx", (code[i + 3] >> 3) & 7, struct.unpack_from("<I", code, i + 4)[0]
    return None


def main():
    data = pathlib.Path(sys.argv[1]).read_bytes()
    table = load_kernel_table()
    base, sections, thunk = parse_xbe(data)

    o = file_offset(sections, thunk)
    imports = []
    while (value := struct.unpack_from("<I", data, o)[0]) != 0:
        if not value & 0x80000000:
            sys.exit(f"import by name? {value:#x}")
        imports.append(value & 0x7FFFFFFF)
        o += 4
    unknown = [x for x in imports if x not in table]
    print(f"kernel table: {len(table)} ordinals; XBE imports {len(imports)}, unknown: {unknown or 'none'}")
    data_imports = [table[x][1] for x in imports if x in table and table[x][0] == "D"]
    print(f"  data exports used ({len(data_imports)}): {', '.join(sorted(data_imports))}")
    funcs = sorted(table[x][1] for x in imports if x in table and table[x][0] == "F")
    print(f"  functions used ({len(funcs)}): {', '.join(funcs)}")

    counts = collections.Counter()
    skipped = []
    for name, flags, va, vsize, raw, rsize in sections:
        if not (flags & 4 and flags & 2):
            continue
        code = data[raw: raw + min(rsize, vsize)]
        i = 0
        while i < len(code):
            site = decode(code, i)
            if not site or site[3] == 0 or site[3] >= KPCR_SIZE:
                i += 1
                continue
            length, kind, reg, offset = site
            if offset == CONTEXT_SLOT or reg == 4:
                skipped.append((va + i, offset, reg))
            else:
                counts[(name, kind, offset)] += 1
            i += length
    print(f"\nfs: reads that would be patched: {sum(counts.values())}, skipped: {skipped or 'none'}")
    for (name, kind, offset), n in sorted(counts.items(), key=lambda kv: (kv[0][2], kv[0][0])):
        print(f"  fs:[{offset:#04x}] {kind:5s} x{n:<3d} in {name}")


if __name__ == "__main__":
    main()
