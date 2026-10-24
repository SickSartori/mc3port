"""Read-only inspector for an Xbox XISO: lists the XDVDFS tree and parses default.xbe."""
import struct, sys, collections

SECTOR = 2048
iso_path = sys.argv[1]
f = open(iso_path, "rb")

def read(off, n):
    f.seek(off); return f.read(n)

base = None
for cand in (0, 0x18300000, 0xFD90000, 0x2080000):
    if read(cand + 32 * SECTOR, 20) == b"MICROSOFT*XBOX*MEDIA":
        base = cand; break
if base is None:
    sys.exit("XDVDFS magic not found")
root_sec, root_size = struct.unpack("<II", read(base + 32 * SECTOR + 20, 8))

files = []  # (path, sector, size, is_dir)

def walk_dir(sec, size, prefix):
    data = read(base + sec * SECTOR, size)
    seen = set()
    def node(off):
        if off in seen or off + 14 > len(data): return
        seen.add(off)
        l, r, s, sz, attr, nl = struct.unpack_from("<HHIIBB", data, off)
        if l == 0xFFFF and r == 0xFFFF: return
        name = data[off + 14: off + 14 + nl].decode("latin-1")
        if l: node(l * 4)
        path = prefix + name
        is_dir = bool(attr & 0x10)
        files.append((path, s, sz, is_dir))
        if is_dir and sz:
            walk_dir(s, sz, path + "/")
        if r: node(r * 4)
    node(0)

walk_dir(root_sec, root_size, "")

total = sum(sz for _, _, sz, d in files if not d)
print(f"partition base=0x{base:X}  files={sum(1 for x in files if not x[3])}  dirs={sum(1 for x in files if x[3])}  total={total/2**20:.1f} MiB")

# top-level summary + extension summary
by_top = collections.Counter(); ext = collections.Counter(); ext_sz = collections.Counter()
for p, s, sz, d in files:
    if d: continue
    by_top[p.split("/")[0] if "/" in p else "(root)"] += sz
    e = p.rsplit(".", 1)[-1].lower() if "." in p.split("/")[-1] else "(none)"
    ext[e] += 1; ext_sz[e] += sz
print("\n== size by top-level entry ==")
for k, v in by_top.most_common(): print(f"  {v/2**20:9.1f} MiB  {k}")
print("\n== extensions ==")
for k, v in ext_sz.most_common(40): print(f"  {ext[k]:6d} files {v/2**20:9.1f} MiB  .{k}")
print("\n== root + dirs (depth<=2) ==")
for p, s, sz, d in files:
    if p.count("/") <= 1:
        print(f"  {'<DIR>' if d else f'{sz:>12}'}  {p}")

# ---- XBE ----
xbe_entry = next((x for x in files if x[0].lower() == "default.xbe"), None)
if not xbe_entry: sys.exit("no default.xbe")
xbe = read(base + xbe_entry[1] * SECTOR, xbe_entry[2])
open(sys.argv[2], "wb").write(xbe) if len(sys.argv) > 2 else None
assert xbe[:4] == b"XBEH"
base_addr, hdr_size, img_size, img_hdr_size, ts, cert_addr, nsec, sec_addr, init_flags, entry, tls = struct.unpack_from("<IIIIIIIIIII", xbe, 0x104)
(stack_commit, heap_reserve, heap_commit, pe_base, pe_size, pe_cksum, pe_ts, dbg_path, dbg_file, dbg_ufile,
 kthunk, nonkern_imp, nlibs, libs_addr, kernlib_addr, xapilib_addr) = struct.unpack_from("<IIIIIIIIIIIIIIII", xbe, 0x130)
def va(a): return a - base_addr
def cstr(a):
    o = va(a); return xbe[o: xbe.index(b"\0", o)].decode("latin-1")
entry_r, entry_d = entry ^ 0xA8FC57AB, entry ^ 0x94859D4B
kt_r, kt_d = kthunk ^ 0x5B6D40B6, kthunk ^ 0xEFB1F152
retail = base_addr <= entry_r < base_addr + img_size
entry, kthunk = (entry_r, kt_r) if retail else (entry_d, kt_d)
title_id, = struct.unpack_from("<I", xbe, va(cert_addr) + 8)
title = xbe[va(cert_addr) + 12: va(cert_addr) + 12 + 80].decode("utf-16-le").rstrip("\0")
region, = struct.unpack_from("<I", xbe, va(cert_addr) + 0xA0)
print(f"\n== default.xbe ({len(xbe)} bytes) ==")
print(f"  title='{title}' title_id=0x{title_id:08X} region=0x{region:X} retail={retail}")
print(f"  base=0x{base_addr:X} image_size=0x{img_size:X} entry=0x{entry:X} kernel_thunk=0x{kthunk:X}")
print(f"  pdb={cstr(dbg_path) if dbg_path else ''}")
print("  sections:")
for i in range(nsec):
    o = va(sec_addr) + i * 56
    flags, vaddr, vsize, raddr, rsize, name_addr = struct.unpack_from("<IIIIII", xbe, o)
    print(f"    {cstr(name_addr):10s} va=0x{vaddr:08X} vsize=0x{vsize:07X} flags=0x{flags:X}")
print("  XDK libraries:")
for i in range(nlibs):
    o = va(libs_addr) + i * 16
    name = xbe[o:o + 8].rstrip(b"\0").decode()
    maj, mnr, bld, fl = struct.unpack_from("<HHHH", xbe, o + 8)
    print(f"    {name:8s} {maj}.{mnr}.{bld}")
# kernel imports (thunk lives in a section: map VA through the section table)
def raw(a):
    for i in range(nsec):
        fl, vaddr, vsize, raddr, rsize, _ = struct.unpack_from("<IIIIII", xbe, va(sec_addr) + i * 56)
        if vaddr <= a < vaddr + rsize: return raddr + (a - vaddr)
    raise ValueError(hex(a))
o = raw(kthunk); n = 0
ords = []
while True:
    v, = struct.unpack_from("<I", xbe, o + 4 * n)
    if v == 0: break
    ords.append(v & 0x7FFFFFFF); n += 1
print(f"  kernel imports: {n}")
print("   ", ords)
