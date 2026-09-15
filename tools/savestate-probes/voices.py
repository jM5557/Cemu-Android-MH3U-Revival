"""AX voice table: which voices are playing, and whether their playback position moves.

Finds the guest AXVPB array by its own shape -- 96 structs of 0x58 bytes whose first
big-endian word counts 0,1,2,... -- then samples each voice's offsets twice. A streamed
voice should show currentOffset advancing and its loop/end window being moved by the game;
a voice stuck looping a fixed buffer shows the window frozen.
"""
import sys, time, struct

sys.path.insert(0, r"E:\cemu_re_mcp\src")
from cemu_re_mcp.pymem_backend import get_bridge
import numpy as np

VPB_SIZE = 0x58
N_VOICES = 96
OFF = 0x34  # AXPBOFFSET_t inside AXVPB

br = get_bridge()
br.attach()
arr = br.read_region()

# index field at +0x00 of each entry: look for 0,1,2,3 four entries in a row
u32 = arr.view(">u4") if arr.dtype == np.uint8 and len(arr) % 4 == 0 else None
base = None
step = VPB_SIZE
for cand in range(0, len(arr) - VPB_SIZE * 8, 4):
    ok = True
    for k in range(6):
        v = struct.unpack(">I", arr[cand + k * step: cand + k * step + 4].tobytes())[0]
        if v != k:
            ok = False
            break
    if ok:
        base = 0x02000000 + cand
        break

if base is None:
    print("AXVPB array not found")
    sys.exit(1)
print(f"AXVPB array at {base:08x}")


def snap():
    out = {}
    for i in range(N_VOICES):
        a = base + i * VPB_SIZE
        b = br.read(a, VPB_SIZE)
        state = struct.unpack(">I", b[0x04:0x08])[0]
        if state == 0:
            continue
        fmt, loopflag = struct.unpack(">HH", b[OFF:OFF + 4])
        loop, end, cur, samples = struct.unpack(">IIII", b[OFF + 4:OFF + 20])
        out[i] = dict(state=state, prio=struct.unpack(">I", b[0x1C:0x20])[0],
                      fmt=fmt, loopflag=loopflag, loop=loop, end=end, cur=cur, samples=samples)
    return out


s0 = snap()
time.sleep(2.0)
s1 = snap()

print(f"{len(s1)} voices playing\n")
print(f"{'v':>3} {'prio':>4} {'fmt':>4} {'lp':>3} {'samples':>9} {'loopOff':>10} {'endOff':>10} {'cur':>10} {'d_cur':>10} {'window moved':>12}")
for i in sorted(s1):
    a, b = s0.get(i), s1[i]
    d = (b["cur"] - a["cur"]) if a else 0
    moved = "yes" if a and (a["loop"] != b["loop"] or a["end"] != b["end"] or a["samples"] != b["samples"]) else "no"
    print(f"{i:>3} {b['prio']:>4} {b['fmt']:>4} {b['loopflag']:>3} {b['samples']:>9x} "
          f"{b['loop']:>10} {b['end']:>10} {b['cur']:>10} {d:>10} {moved:>12}")
