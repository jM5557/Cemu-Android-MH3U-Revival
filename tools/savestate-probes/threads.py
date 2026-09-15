"""Dump every guest OSThread_t by scanning for its magic, and sample twice.

Reads the scheduler's own structures rather than guessing a game field that happens to
tick: a thread's state, run-queue links and wait queue say directly whether the guest is
executing, blocked, or simply never scheduled.
"""
import sys, time, struct

sys.path.insert(0, r"E:\cemu_re_mcp\src")
from cemu_re_mcp.pymem_backend import get_bridge
import numpy as np

MAGIC = 0x74487244  # "tHrD" at OSThread_t+0x320

STATE = {0: "NONE", 1: "READY", 2: "RUNNING", 4: "WAITING", 8: "MORIBUND"}

br = get_bridge()
info = br.attach()
print(info)

arr = br.read_region()  # uint8 view of guest space
u32be = arr.view(np.uint8)

# find magic occurrences
pat = np.frombuffer(struct.pack(">I", MAGIC), dtype=np.uint8)
cand = np.where((u32be[:-3] == pat[0]) & (u32be[1:-2] == pat[1]) &
                (u32be[2:-1] == pat[2]) & (u32be[3:] == pat[3]))[0]
print(f"magic hits: {len(cand)}")

threads = []
for off in cand:
    base = 0x02000000 + int(off) - 0x320
    if base < 0:
        continue
    threads.append(base)

GUEST_BASE = 0  # read_region is indexed by guest address directly in this backend


def snap():
    out = {}
    for t in threads:
        try:
            b = br.read(t + 0x320, 0x48)
        except Exception:
            continue
        state = b[0x04]
        attr = b[0x05]
        tid = struct.unpack(">H", b[0x06:0x08])[0]
        susp = struct.unpack(">i", b[0x08:0x0C])[0]
        prio = struct.unpack(">i", b[0x0C:0x10])[0]
        rq = [struct.unpack(">I", b[0x18 + i * 4:0x1C + i * 4])[0] for i in range(3)]
        wq = struct.unpack(">I", b[0x3C:0x40])[0]
        out[t] = (state, attr, tid, susp, prio, tuple(rq), wq)
    return out


a = snap()
time.sleep(2.0)
b = snap()

print(f"\n{'addr':>10} {'id':>4} {'state':>9} {'aff':>4} {'susp':>5} {'prio':>4} {'runq':>26} {'waitq':>9}  chg")
nready = nrun = nwait = 0
for t in sorted(a):
    st, attr, tid, susp, prio, rq, wq = a[t]
    changed = "CHANGED" if a[t] != b.get(t) else ""
    name = STATE.get(st, f"?{st:02x}")
    if st == 1:
        nready += 1
    elif st == 2:
        nrun += 1
    elif st == 4:
        nwait += 1
    rqs = ",".join(f"{x:08x}" for x in rq)
    print(f"0x{t:08x} {tid:>4} {name:>9} {attr & 7:>4} {susp:>5} {prio:>4} {rqs:>26} {wq:08x}  {changed}")

print(f"\nREADY={nready} RUNNING={nrun} WAITING={nwait} total={len(a)}")
print(f"threads whose scheduler fields changed over 2s: {sum(1 for t in a if a[t] != b.get(t))}")
