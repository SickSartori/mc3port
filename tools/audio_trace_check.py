"""Lifecycle of voices from MC3_AUDIO_TRACE lines: how long a loop lives, how many copies of one sound play at once."""
import re, sys
from collections import defaultdict

line_re = re.compile(
    r"audio: t (?P<t>[\d.]+) (?P<what>\w+) (?P<flags>[0-9a-f]+) (?P<kind>buffer|stream) (?P<voice>[0-9A-F]+) data (?P<data>[0-9A-F]+) "
    r"fmt (?P<tag>\d+)/(?P<rate>\d+) Hz ch \d+ vol (?P<vol>-?\d+) pitch (?P<pitch>-?\d+) loop (?P<loop>\d) region (?P<start>\d+)\+(?P<len>\d+) "
    r"pos (?P<pos>-?\d+) age (?P<age>[\d.]+) s bins (?P<bins>-?\d+)")
events = []
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = line_re.search(line)
    if m:
        d = m.groupdict()
        d["t"] = float(d["t"])
        events.append(d)
print(f"{len(events)} trace events")
if not events:
    sys.exit()
t0 = events[0]["t"]

# Lifecycle per voice: a play opens an instance, stop/stopex/released/ended closes it.
open_at = {}
lives = defaultdict(list)  # sound key -> [(start, end, how)]
stops = defaultdict(int)
for e in events:
    key = (e["data"], e["start"], e["len"], e["rate"])
    v = e["voice"]
    if e["what"] == "play":
        if v in open_at:  # replay while playing
            s, k = open_at[v]
            lives[k].append((s, e["t"], "replay"))
        open_at[v] = (e["t"], key)
    else:
        stops[e["what"] + " " + e["flags"]] += 1
        if v in open_at:
            s, k = open_at.pop(v)
            lives[k].append((s, e["t"], e["what"]))
for v, (s, k) in open_at.items():
    lives[k].append((s, events[-1]["t"], "still playing"))
print("how instances end:", dict(stops))

# Concurrency per sound: the most copies alive at once, and the longest-lived copy.
print("\nsounds with the most simultaneous copies (data, region, rate): max copies, instances, median life s, ends")
rows = []
for k, spans in lives.items():
    points = sorted([(s, 1) for s, _, _ in spans] + [(e, -1) for _, e, _ in spans])
    alive = peak = 0
    for _, d in points:
        alive += d
        peak = max(peak, alive)
    durations = sorted(e - s for s, e, _ in spans)
    ends = defaultdict(int)
    for _, _, how in spans:
        ends[how] += 1
    rows.append((peak, len(spans), durations[len(durations) // 2], k, dict(ends)))
rows.sort(reverse=True)
for peak, n, med, k, ends in rows[:15]:
    print(f"  {peak:3d} copies  {n:4d} inst  life {med:6.2f} s  data {k[0]} region {k[1]}+{k[2]} {k[3]} Hz  {ends}")

# Play flags seen, and stop lag: time from a stopex to the release end of the same voice.
flags = defaultdict(int)
for e in events:
    if e["what"] == "play":
        flags[e["flags"]] += 1
print("\nplay flags:", dict(flags))
