"""Is the game still writing into its streamed-audio buffers?

A looping voice plays whatever is in its sample buffer; the game is expected to refill
that buffer ahead of the play cursor. Hashing the buffer over a few seconds separates
"the stream is dead and the voice is looping stale samples" from "the stream is fine and
the fault is downstream".
"""
import sys, time, zlib, struct

sys.path.insert(0, r"E:\cemu_re_mcp\src")
from cemu_re_mcp.pymem_backend import get_bridge

br = get_bridge()
br.attach()

addrs = [int(a, 16) for a in sys.argv[1:]] or [0x2dda4b40]
SPAN = 0x20000  # 128 KiB window per buffer


def hashes():
    return [zlib.crc32(br.read(a, SPAN)) for a in addrs]


h0 = hashes()
time.sleep(4.0)
h1 = hashes()
for a, x, y in zip(addrs, h0, h1):
    print(f"{a:08x}  {x:08x} -> {y:08x}   {'CHANGED' if x != y else 'static'}")
