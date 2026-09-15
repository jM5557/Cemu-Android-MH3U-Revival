"""Screenshot Cemu's window and compare successive frames.

Only the game viewport is compared. The title bar carries a live FPS readout, so a
full-window diff reports "frames are changing" even when the picture is frozen --
that mistake sent the last investigation down the wrong path.
"""
import ctypes, sys, time
from ctypes import wintypes
from PIL import ImageGrab

user32 = ctypes.windll.user32

VIEWPORT = (10, 55, 1280, 740)  # relative to window rect: excludes title bar + borders


def find_cemu():
    out = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            t = buf.value
            if t.startswith("Cemu ") and "TitleId:" in t and user32.IsWindowVisible(hwnd):
                out.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return out


hwnd = find_cemu()[0]
user32.SetForegroundWindow(hwnd)
time.sleep(0.6)

rect = wintypes.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(rect))

n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
gap = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
tag = sys.argv[3] if len(sys.argv) > 3 else "shot"

shots = []
for i in range(n):
    img = ImageGrab.grab((rect.left, rect.top, rect.right, rect.bottom))
    vp = img.crop(VIEWPORT)
    path = f"{tag}_{i}.png"
    vp.save(path)
    shots.append(vp.tobytes())
    print(f"saved {path}")
    if i != n - 1:
        time.sleep(gap)

print()
for i in range(1, n):
    same = shots[i] == shots[0]
    diff = sum(1 for a, b in zip(shots[i], shots[0]) if a != b)
    pct = 100.0 * diff / len(shots[0])
    print(f"frame {i} vs 0: {'IDENTICAL (frozen)' if same else f'DIFFERS in {pct:.2f}% of bytes'}")
