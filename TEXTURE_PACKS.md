# Custom texture packs

Cemu can replace a game's textures with your own DDS files while the game runs.

## Where packs go

```
<Cemu user folder>/load/textures/<title ID>/<pack name>/
```

Each folder inside the game's title ID folder is one pack, and you can switch packs on and off per game
(right-click the game in the list, *Custom textures*). When two enabled packs replace the same texture,
the pack whose name comes first alphabetically wins.

## Finding a texture's hash

Turn on texture dumping (Debug > Dump > Textures) and play to where the texture appears. Dumps are
written to `dump/textures/`, named like this:

```
22fb5c1e481b3338_1280x720_fmt0031_mip00.tga
```

The first 16 characters are the texture's **hash**. That is what a replacement is matched by.

## Naming files after their hash

The simplest pack is a folder of DDS files named exactly like the dumps, with `.dds` instead of `.tga`:

```
22fb5c1e481b3338_1280x720_fmt0031_mip00.dds
```

Subfolders are fine; the whole pack folder is searched.

## textures.ini: any file names you like

To keep a pack organised, put a `textures.ini` at the top of the pack folder and name your files however
you want. The format follows PPSSPP's:

```ini
[hashes]
# Menus
22fb5c1e481b3338 = menu/background1.dds
802f4997f61d5ec3 = menu/background2.dds
b000e9658445a9ae = menu/capcom.dds

# GUI
6a0d3c21f9e4b7a5 = GUI/item_icons.dds

[filtering]
b000e9658445a9ae = linear
6a0d3c21f9e4b7a5 = nearest
```

- **Keys** are the 16-character hash from the dump filename. A plain hash means mip level 0; a single
  DDS with a full mip chain covers every level. To give a separate file for another mip level, use the
  whole dump name as the key, e.g. `22fb5c1e481b3338_1280x720_fmt0031_mip01 = menu/background1_mip1.dds`.
  A pack made before the hash change (its files named with the older hashes) also needs the whole dump
  name, because an older hash is only accepted for a texture of the size and format in that name.
- **Paths** are relative to the pack folder, and must stay inside it (no `..` or drive letters). `/` and
  `\` both work.
- **Comments** start with `#`, on their own line or after an entry. (So a file name cannot contain `#`.)
- A `textures.ini` entry beats a hash-named file in the same pack. Hash-named files keep working alongside
  it, so you can move an existing pack over a bit at a time.
- Problems (a hash that is not a hash, a missing file, a filter that is not recognised) are written to
  `log.txt` with the line number, and that line is skipped.

### [filtering]

Forces how a texture is sampled, overriding what the game asks for:

| Value | Effect |
|---|---|
| `nearest` | Sharp, unblended pixels. Good for pixel art and fonts. Also turns off anisotropic filtering for that texture. |
| `linear` | Smooth, blended. |
| `auto` | The game's own filtering (the same as leaving the texture out). |

A filter applies to the game's texture whether or not the pack also replaces it, so you can fix the
filtering of an original texture without shipping a replacement. It works on Vulkan, OpenGL and Metal.
It covers the textures a pack can replace (BCn-compressed textures, and RGBA8 textures the game loads
from its files), not render targets.

If two enabled packs set a filter for the same hash, the first pack alphabetically wins, as with files.
