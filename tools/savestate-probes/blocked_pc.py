"""For every WAITING guest thread, what guest PC does its saved context hold?

This decides the design. If a blocked thread's context points at the HLE stub it called,
the thread can be restarted by re-invoking that call after a load -- a portable fix. If it
points somewhere unrelated, the host fiber stack is the only record of where it was, and
only a host-stack snapshot can bring it back.
"""
import sys, struct

sys.path.insert(0, r"E:\cemu_re_mcp\src")
from cemu_re_mcp.pymem_backend import get_bridge
import numpy as np

MAGIC = 0x74487244
STATE = {0: "NONE", 1: "READY", 2: "RUNNING", 4: "WAITING", 8: "MORIBUND"}

br = get_bridge()
br.attach()
arr = br.read_region()

pat = np.frombuffer(struct.pack(">I", MAGIC), dtype=np.uint8)
cand = np.where((arr[:-3] == pat[0]) & (arr[1:-2] == pat[1]) &
                (arr[2:-1] == pat[2]) & (arr[3:] == pat[3]))[0]

print(f"{'thread':>10} {'state':>9} {'srr0':>10} {'lr':>10} {'sp(r1)':>10} {'r3':>10} {'r4':>10}")
seen = []
for off in cand:
    t = 0x02000000 + int(off) - 0x320
    ctx = br.read(t, 0x100)
    if struct.unpack(">I", ctx[0:4])[0] != 0x4F53436F:
        continue  # not a real context ("OSCo")
    st = br.read(t + 0x324, 1)[0]
    gpr = struct.unpack(">32I", ctx[0x08:0x88])
    lr = struct.unpack(">I", ctx[0x8C:0x90])[0]
    srr0 = struct.unpack("<I", ctx[0x98:0x9C])[0]  # stored host-endian, unlike the rest of OSContext_t
    if st != 4:
        continue
    seen.append(srr0)
    print(f"0x{t:08x} {STATE.get(st, st):>9} 0x{srr0:08x} 0x{lr:08x} 0x{gpr[1]:08x} 0x{gpr[3]:08x} 0x{gpr[4]:08x}")

print(f"\nwaiting threads: {len(seen)}")
print(f"distinct srr0 values: {len(set(seen))}")
for v in sorted(set(seen)):
    print(f"  0x{v:08x}  x{seen.count(v)}")
