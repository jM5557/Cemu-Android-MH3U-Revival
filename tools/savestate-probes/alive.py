"""Unambiguous liveness: is Cemu's scheduler actually dispatching guest threads?

OSThread_t.wakeUpCount (+0x608) is incremented in __OSLoadThread every time a thread
enters the running state, and totalCycles (+0x610) accumulates executed cycles. Both are
monotonic and driven only by real execution, so their delta over a wall-clock interval
answers the question outright.

Why not the earlier probes: a state/run-queue snapshot looks identical for a healthy game
and a stopped one (at any instant most threads are waiting), and a game field like the
hunger timer does not tick in every area.
"""
import sys, time, struct

sys.path.insert(0, r"E:\cemu_re_mcp\src")
from cemu_re_mcp.pymem_backend import get_bridge
import numpy as np

MAGIC = 0x74487244

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
        if struct.unpack(">I", br.read(t, 4))[0] == 0x4F53436F:  # "OSCo"
            threads.append(t)
    except Exception:
        pass


def totals():
    wake = cyc = 0
    per = {}
    for t in threads:
        b = br.read(t + 0x608, 16)
        w = struct.unpack(">Q", b[0:8])[0]
        c = struct.unpack(">Q", b[8:16])[0]
        wake += w
        cyc += c
        per[t] = w
    return wake, cyc, per


w0, c0, p0 = totals()
time.sleep(3.0)
w1, c1, p1 = totals()

moved = sum(1 for t in threads if p1[t] != p0[t])
print(f"threads tracked      : {len(threads)}")
print(f"wakeUpCount total    : {w0} -> {w1}   delta={w1 - w0}")
print(f"totalCycles total    : {c0} -> {c1}   delta={c1 - c0}")
print(f"threads that ran     : {moved}")
print()
if w1 - w0 == 0 and c1 - c0 == 0:
    print(">>> GUEST IS STOPPED - no thread was dispatched in 3 seconds")
else:
    print(">>> GUEST IS RUNNING")
    top = sorted(((p1[t] - p0[t], t) for t in threads), reverse=True)[:6]
    for d, t in top:
        if d:
            print(f"    0x{t:08x}  +{d} timeslices")
