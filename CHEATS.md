# Cheats

Cemu can load Gateway-style cheat codes per game, in the same file layout Citra/Azahar use for 3DS cheats.

## Where to find it

**Android**
- **Game list:** long-press a game → **Cheats**. Works whether or not the game is running.
- **While playing:** open the side menu (Back) → **Cheats**.

Flip a cheat's switch to enable it; the file is saved immediately and, if the game is running, the cheat starts or stops on the next frame. Tap a cheat to edit its name, notes or code, or to delete it; **+** adds a new one. Code that doesn't parse is rejected with the line number and reason. The refresh button re-reads the file, for cheat files edited or copied in by hand.

**Desktop**
- **Game list:** right-click a game → **Cheats**.
- **While playing:** **Tools → Cheats**.

## File format

Files live in `<Cemu user data>/cheats/<titleId>.txt` (the Cheats window shows the exact path):

```
[Equipment box in Port Tanzia tavern]
{Wii U port of the 3DS code}
*cemu_enabled
02XXXXXX 38A00000
```

- `[name]` starts a cheat.
- `{notes}` is optional and may span several lines.
- `*cemu_enabled` marks the cheat as on. `*citra_enabled` is accepted too.
- Each code line is two 8-digit hex values.

Enabled cheats run once per vsync (about 60 times a second).

## Differences from 3DS codes

The syntax is the same, but the codes themselves must be written for the Wii U game:

- **Addresses are Wii U addresses.** A 3DS address means nothing on the Wii U; the game has to be reverse engineered again.
- **Memory is big-endian.** `0XXXXXXX 38A00000` stores the bytes `38 A0 00 00` in that order, which is how a PowerPC instruction is encoded. Instruction patches therefore use PowerPC machine code, e.g. `38A00000` = `li r5, 0`.
- **Addresses above `0x0FFFFFFF` need the offset register.** Codes only carry 28 address bits. Game code (0x02000000+) is reachable directly; heap memory (0x10000000+) is reached like this:
  ```
  D3000000 10000000
  00123450 0000270F
  ```
  which writes 9999 to `0x10123450`.
- **Button conditions (`DD`) are not supported.** They are tied to the 3DS button layout.
- **Code writes take effect immediately.** When a cheat changes bytes in the code region, Cemu discards its recompiled copy of that code so the new instruction runs. Writes that don't change anything are skipped, so constant patches cost nothing after the first frame.

## Supported code types

| Code | Meaning |
|---|---|
| `0XXXXXXX YYYYYYYY` | 32-bit write `[X+offset] = Y` |
| `1XXXXXXX 0000YYYY` | 16-bit write |
| `2XXXXXXX 000000YY` | 8-bit write |
| `3XXXXXXX YYYYYYYY` | if `[X+offset] < Y` |
| `4XXXXXXX YYYYYYYY` | if `[X+offset] > Y` |
| `5XXXXXXX YYYYYYYY` | if `[X+offset] == Y` |
| `6XXXXXXX YYYYYYYY` | if `[X+offset] != Y` |
| `7`–`AXXXXXXX ZZZZYYYY` | same comparisons, 16-bit, ignoring the bits set in mask `Z` |
| `BXXXXXXX 00000000` | `offset = [X+offset]` (pointer) |
| `C0000000 YYYYYYYY` | repeat the block up to the next `D1`/`D2` Y times |
| `D0000000 00000000` | end if |
| `D1000000 00000000` | end loop |
| `D2000000 00000000` | end loop, end all ifs, clear offset and data |
| `D3000000 YYYYYYYY` | `offset = Y` |
| `D4000000 YYYYYYYY` | `data += Y` |
| `D5000000 YYYYYYYY` | `data = Y` |
| `D6`/`D7`/`D8000000 YYYYYYYY` | write data (32/16/8-bit) to `[Y+offset]`, then advance offset |
| `D9`/`DA`/`DB000000 YYYYYYYY` | load data (32/16/8-bit) from `[Y+offset]` |
| `DC000000 YYYYYYYY` | `offset += Y` |
| `EXXXXXXX YYYYYYYY` | copy the Y bytes written on the following lines to `X+offset` |

If a code touches an unmapped address, that run stops, the problem is written to `log.txt` once, and the cheat is retried on the next frame.

## Very short functions

Cemu's recompiler copies functions of a few instructions that end in `blr` (for example a getter or setter such as `stw r4, 0x1F8(r3)` / `blr`) straight into the code that calls them. A cheat that patches such a function changes the original, but callers that were already compiled keep using their own copy, so the patch appears to do nothing. Patch the caller instead, or use a graphic pack patch, which is applied before any code is compiled.

