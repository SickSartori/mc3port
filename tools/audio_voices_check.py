"""MC3_AUDIO_VOICES snapshots: per 10 s block, the voices heard through XTLK bins (6-9) and the loudest voices."""
import re, sys
from collections import defaultdict

head = re.compile(r"audio: last 10 s: .*up to (\d+) voices")
voice = re.compile(
    r"^\s*voice ([0-9A-F]+) (buffer|stream) flags \S+ fmt (\S+) Hz/\d+ bit ch (\d) vol (-?\d+) pitch (-?\d+) loop (\d) "
    r"size \d+ region (\S+) loop \S+ pos \S+ age ([\d.]+) s gain L ([\d.]+) R ([\d.]+) bins(.*)$")
blocks, current = [], None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = head.search(line)
    if m:
        current = {"most": int(m.group(1)), "voices": []}
        blocks.append(current)
        continue
    m = voice.match(line)
    if m and current is not None:
        bins = [(int(b), int(v)) for b, v in re.findall(r"(\d+):(-?\d+)", m.group(12))]
        current["voices"].append(dict(id=m.group(1)[-4:], fmt=m.group(3), vol=int(m.group(5)), pitch=int(m.group(6)),
                                      loop=m.group(7), region=m.group(8), age=float(m.group(9)),
                                      L=float(m.group(10)), R=float(m.group(11)), bins=bins))
print(f"{len(blocks)} snapshots")
for i, b in enumerate(blocks):
    if b["most"] < 15:
        continue
    xtlk = [v for v in b["voices"] if any(6 <= bn <= 9 and vol > -6000 for bn, vol in v["bins"])]
    loud = sorted(b["voices"], key=lambda v: -(v["L"] + v["R"]))[:5]
    print(f"\n-- block {i} ({b['most']} voices at most, {len(b['voices'])} listed, {len(xtlk)} through XTLK)")
    for v in xtlk[:8]:
        bins = " ".join(f"{bn}:{vol}" for bn, vol in v["bins"] if vol > -6000)
        print(f"  XTLK {v['id']} {v['fmt']} vol {v['vol']} pitch {v['pitch']} loop {v['loop']} reg {v['region']} age {v['age']:.1f} L {v['L']:.2f} R {v['R']:.2f} | {bins}")
    for v in loud:
        print(f"  loud {v['id']} {v['fmt']} vol {v['vol']} pitch {v['pitch']} loop {v['loop']} reg {v['region']} age {v['age']:.1f} L {v['L']:.2f} R {v['R']:.2f} bins {len(v['bins'])}")
