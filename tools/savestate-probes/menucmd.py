"""Post a wx menu command straight to Cemu's main window.

More reliable than driving the menu with synthetic keystrokes: the emulator
grabs keyboard input, so Alt+F only lands sometimes.
"""
import ctypes, sys
from ctypes import wintypes

WM_COMMAND = 0x0111
user32 = ctypes.windll.user32

def find_cemu():
    out = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            t = buf.value
            # Must be the emulator window with a game running, not just any window
            # whose title starts with "Cemu" -- the editor showing this branch does too.
            if t.startswith("Cemu ") and "TitleId:" in t and user32.IsWindowVisible(hwnd):
                out.append((hwnd, t))
        return True
    user32.EnumWindows(cb, 0)
    return out

cmd_id = int(sys.argv[1])
wins = find_cemu()
if not wins:
    print("no Cemu window found"); sys.exit(1)
hwnd, title = wins[0]
print(f"hwnd=0x{hwnd:X} title={title!r}")
ok = user32.PostMessageW(hwnd, WM_COMMAND, cmd_id, 0)
print(f"PostMessage WM_COMMAND id={cmd_id} -> {bool(ok)}")
