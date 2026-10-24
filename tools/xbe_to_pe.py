"""Wraps the sections of an XBE into a minimal PE file so standard tools (dumpbin /disasm, IDA, x64dbg...) can
read the game's code at its real addresses. The result is for analysis only and cannot run.

  python -I tools/xbe_to_pe.py <default.xbe> <out.exe>
  dumpbin /disasm /range:0x13600,0x13700 <out.exe>
"""
import struct
import sys

FILE_ALIGN = 0x200
SECTION_ALIGN = 0x20  # XBE sections are packed tighter than pages; keep their exact addresses


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def main():
    xbe = open(sys.argv[1], "rb").read()
    u32 = lambda o: struct.unpack_from("<I", xbe, o)[0]
    base, header_size = u32(0x104), u32(0x108)
    entry = u32(0x128) ^ 0xA8FC57AB
    if not base <= entry < base + u32(0x10C):
        entry = u32(0x128) ^ 0x94859D4B

    sections = []
    for i in range(u32(0x11C)):
        o = u32(0x120) - base + i * 56
        flags, va, vsize, raw, rsize, name_addr = struct.unpack_from("<IIIIII", xbe, o)
        name = xbe[name_addr - base: xbe.index(b"\0", name_addr - base)][:8]
        sections.append((name, flags, va, vsize, xbe[raw: raw + rsize]))
    sections.sort(key=lambda s: s[2])

    headers_size = align(0x40 + 4 + 20 + 0xE0 + 40 * len(sections), FILE_ALIGN)
    out = bytearray(headers_size)
    struct.pack_into("<2s58xI", out, 0, b"MZ", 0x40)
    out[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", out, 0x44, 0x14C, len(sections), 0, 0, 0, 0xE0, 0x0102 | 0x0001)
    image_size = align(max(va + vsize for _, _, va, vsize, _ in sections) - base, 0x1000)
    opt = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", 0x10B, 14, 0, 0, 0, 0, entry - base, 0, 0, base,
                      SECTION_ALIGN, FILE_ALIGN, 4, 0, 0, 0, 4, 0, 0, image_size, headers_size, 0, 2, 0,
                      0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    out[0x58:0x58 + len(opt)] = opt
    table = 0x58 + 0xE0
    raw_cursor = headers_size
    blobs = []
    for i, (name, flags, va, vsize, data) in enumerate(sections):
        characteristics = 0x40000000  # readable
        characteristics |= 0x20000020 if flags & 4 else 0x40  # code+exec, or initialised data
        if flags & 1:
            characteristics |= 0x80000000
        struct.pack_into("<8sIIIIIIHHI", out, table + i * 40, name, vsize, va - base, align(len(data), FILE_ALIGN),
                         raw_cursor, 0, 0, 0, 0, characteristics)
        blobs.append(data + b"\0" * (align(len(data), FILE_ALIGN) - len(data)))
        raw_cursor += len(blobs[-1])
    with open(sys.argv[2], "wb") as f:
        f.write(out)
        for blob in blobs:
            f.write(blob)


if __name__ == "__main__":
    main()
