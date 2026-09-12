#!/usr/bin/env python3
"""
Rename a custom texture pack from the pre-discriminator hash scheme to the current one.

Why this is needed: the replacement lookup key used to be a plain content hash that ignored the
surface's dimensions and format, so two different textures of the same size could map to the same
pack entry and the wrong image would be applied. Folding those discriminators in fixes that, but
it changes every hash, and the hash is the filename.

Usage:
    1. Build with the fix, turn texture dumping on, and play through the areas your pack covers.
       Every texture seen appends one line to <UserData>/dump/textures/rename_map.csv.
    2. Pull it off the device:
         adb exec-out run-as info.cemu.cemu cat files/dump/textures/rename_map.csv > rename_map.csv
       (add .debug to the package name for a debug build; if run-as is refused, copy the file out
       through the app's own Documents provider instead)
    3. Dry run, then apply:
         python3 migrate_texture_pack.py rename_map.csv /path/to/pack
         python3 migrate_texture_pack.py rename_map.csv /path/to/pack --apply

Only the hash changes. Dimensions, format and mip index in the filename are preserved, so a file
that does not appear in the map is left alone and reported at the end.
"""

import argparse
import csv
import os
import pathlib
import sys


def load_map(csv_path):
    """Returns {old_stem: new_stem}. Later duplicates are ignored, not overwritten."""
    mapping = {}
    conflicts = 0
    with open(csv_path, newline="") as handle:
        for row in csv.reader(handle):
            if len(row) != 2:
                continue
            old, new = row[0].strip(), row[1].strip()
            if not old or not new:
                continue
            if old in mapping and mapping[old] != new:
                conflicts += 1
                continue
            mapping[old] = new
    if conflicts:
        print(f"warning: {conflicts} entries mapped one old name to several new ones; kept the first")
    return mapping


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rename_map", help="rename_map.csv produced by a dumping run")
    parser.add_argument("pack_dir", help="pack folder to rename in place (searched recursively)")
    parser.add_argument("--apply", action="store_true", help="actually rename; without this it is a dry run")
    parser.add_argument("--ext", default=".dds", help="file extension to rename (default .dds)")
    args = parser.parse_args()

    mapping = load_map(args.rename_map)
    if not mapping:
        sys.exit("rename_map.csv contained no usable entries")
    print(f"loaded {len(mapping)} mappings")

    pack = pathlib.Path(args.pack_dir)
    if not pack.is_dir():
        sys.exit(f"{pack} is not a directory")

    renamed, unmatched, collisions = 0, [], []
    for path in sorted(pack.rglob("*" + args.ext)):
        new_stem = mapping.get(path.stem)
        if new_stem is None:
            unmatched.append(path)
            continue
        target = path.with_name(new_stem + path.suffix)
        if target.exists() and target != path:
            collisions.append((path, target))
            continue
        print(f"{path.name}  ->  {target.name}")
        if args.apply:
            os.rename(path, target)
        renamed += 1

    print()
    print(f"{'renamed' if args.apply else 'would rename'}: {renamed}")
    if collisions:
        print(f"skipped (target already exists): {len(collisions)}")
        for src, dst in collisions[:10]:
            print(f"  {src.name} -> {dst.name}")
    if unmatched:
        # Usually means that texture was never displayed during the dumping run. Play through the
        # area it belongs to and re-run; the map is appended to, so earlier entries are kept.
        print(f"not in the map, left alone: {len(unmatched)}")
        for path in unmatched[:10]:
            print(f"  {path.name}")
        if len(unmatched) > 10:
            print(f"  ... and {len(unmatched) - 10} more")
    if not args.apply:
        print("\ndry run; re-run with --apply to rename")


if __name__ == "__main__":
    main()
