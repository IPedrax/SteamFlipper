#!/usr/bin/env python3
"""Uncomment the depot manifest pins whose manifest file is already on disk.

    tools/pin_manifests.py            # apply
    tools/pin_manifests.py --dry-run  # report what would change
    tools/pin_manifests.py --steam ~/.steam/steam

Manifest packs ship their setManifestid lines commented out, which is right for
a game you own: Steam fetches whatever version is current. With injected
ownership it is wrong, because fetching a version needs a manifest request code
from Valve and Valve refuses for an account that does not own the app. Steam
logs that as 'Access Denied' and the client shows it as "no internet
connection", or stops with the content still encrypted.

The archived manifest that came with the pack is the one that can be used, and
it is sitting in depotcache. Uncommenting the pin points Steam at it, and no
request code is needed because the file is already there.

Only a pin whose file is present is uncommented. Pinning a version that is not
on disk would send Steam back to Valve for that one instead, which fails the
same way, so those are left alone and counted separately.

No restart needed: Steam does not own these files, and the module's watcher
reloads them.
"""
import argparse
import pathlib
import re
import shutil
import sys

# A commented pin. The size argument is optional and is ignored by the loader,
# which forces it to 0, so it is captured only to be written back untouched.
COMMENTED = re.compile(
    r'^([ \t]*)--[ \t]*(setManifestid\(\s*(\d+)\s*,\s*"(\d+)".*)$',
    re.M | re.I)


def default_steam_dir() -> pathlib.Path:
    for candidate in ("~/.local/share/Steam", "~/.steam/steam", "~/.steam/root"):
        p = pathlib.Path(candidate).expanduser()
        if (p / "config" / "config.vdf").is_file():
            return p
    sys.exit("could not find a Steam directory; pass --steam")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--steam", type=pathlib.Path, default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    steam = args.steam.expanduser() if args.steam else default_steam_dir()
    lua_dir = steam / "config" / "stplug-in"
    depotcache = steam / "depotcache"
    if not lua_dir.is_dir():
        print(f"  no Lua manifest directory yet ({lua_dir}); nothing to pin")
        return 0

    changed_files = 0
    pinned = 0
    skipped_absent = 0

    for path in sorted(lua_dir.glob("*.lua")):
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        hits = []

        for m in COMMENTED.finditer(text):
            indent, call, depot, gid = m.group(1), m.group(2), m.group(3), m.group(4)
            if (depotcache / f"{depot}_{gid}.manifest").is_file():
                hits.append((m.group(0), indent + call, depot, gid))
            else:
                skipped_absent += 1

        if not hits:
            continue

        new_text = text
        for old_line, new_line, _depot, _gid in hits:
            new_text = new_text.replace(old_line, new_line, 1)

        changed_files += 1
        pinned += len(hits)
        print(f"  {path.name}: {len(hits)} pin(s)")
        for _old, _new, depot, gid in hits[:4]:
            print(f"      depot {depot} -> {gid}")
        if len(hits) > 4:
            print(f"      ... and {len(hits) - 4} more")

        if not args.dry_run:
            # Kept beside the file, once, the way the fix extractor does it, so
            # a pack can be put back exactly as it shipped.
            backup = path.with_suffix(".lua.sf-orig")
            if not backup.exists():
                shutil.copy2(path, backup)
            path.write_text(new_text, encoding="utf-8", errors="surrogateescape")

    print()
    print(f"  files changed      : {changed_files}")
    print(f"  pins uncommented   : {pinned}")
    print(f"  left commented     : {skipped_absent}  (no manifest on disk for them)")
    if args.dry_run and pinned:
        print("  dry run; nothing written")
    elif pinned:
        print("  originals kept alongside as <name>.lua.sf-orig")
    return 0


if __name__ == "__main__":
    sys.exit(main())
