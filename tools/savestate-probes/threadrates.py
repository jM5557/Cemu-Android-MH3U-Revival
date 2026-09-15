"""Per-thread execution rates, as JSON, for before/after comparison across a state load.

alive.py answers "is anything running". This answers "which threads are running, and at
what rate" -- which is what you need when the world is alive but one subsystem is not.
Take a sample before a save and another after the load, then diff: a thread that was
ticking before and is flat after, together with the queue it is parked on, names the
thing that never got signalled.

Usage:  python threadrates.py <out.json> [seconds]
"""
import sys, time, struct, json

sys.path.insert(0, r"E:\cemu_re_mcp\src")
from cemu_re_mcp.pymem_backend import get_bridge
import numpy as np

MAGIC = 0x74487244
STATE_NAMES = {0: "NONE", 1: "READY", 2: "RUNNING", 4: "WAITING", 8: "MORIBUND"}

out_path = sys.argv[1] if len(sys.argv) > 1 else "threadrates.json"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

br = get_bridge()
br.attach()
arr = br.read_region()
pat = np.frombuffer(struct.pack(">I", MAGIC), dtype=np.uint8)
cand = np.where((arr[:-3] == pat[0]) & (arr[1:-2] == pat[1]) &
                (arr[2:-1] == pat[2]) & (arr[3:] == pat[3]))[0]

threads = []
for off in cand:
    t = 0x02000000 + int(off) - 0x320
    try:
        if struct.unpack(">I", br.read(t, 4))[0] == 0x4F53436F:  # "OSCo" context magic
            threads.append(t)
    except Exception:
        pass


def read_name(ptr):
    if not ptr:
        return None
    try:
        raw = br.read(ptr, 64)
        return raw.split(b"\0")[0].decode("ascii", "replace") or None
    except Exception:
        return None


def sample():
    out = {}
    for t in threads:
        b = br.read(t + 0x608, 16)
        out[t] = (struct.unpack(">Q", b[0:8])[0], struct.unpack(">Q", b[8:16])[0])
    return out


s0 = sample()
time.sleep(duration)
s1 = sample()

rows = []
for t in threads:
    dw = s1[t][0] - s0[t][0]
    dc = s1[t][1] - s0[t][1]
    state = struct.unpack(">B", br.read(t + 0x324, 1))[0]
    rows.append({
        "addr": f"{t:08x}",
        "d_wake": dw,
        "d_cycles": dc,
        "wake": s1[t][0],
        "state": STATE_NAMES.get(state, str(state)),
        "prio": struct.unpack(">i", br.read(t + 0x32C, 4))[0],
        "waitq": f"{struct.unpack('>I', br.read(t + 0x35C, 4))[0]:08x}",
        "name": read_name(struct.unpack(">I", br.read(t + 0x5C0, 4))[0]),
    })

rows.sort(key=lambda r: -r["d_wake"])
with open(out_path, "w") as f:
    json.dump({"duration": duration, "threads": rows}, f, indent=1)

active = [r for r in rows if r["d_wake"]]
print(f"{len(threads)} threads, {len(active)} ran in {duration:.0f}s -> {out_path}")
for r in active:
    print(f"  {r['addr']}  +{r['d_wake']:<7} wake  {r['d_cycles']:>14} cyc  "
          f"prio {r['prio']:<3} {r['name'] or ''}")
