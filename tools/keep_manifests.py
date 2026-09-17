#!/usr/bin/env python3
"""Keep depot manifests alive across installs, and put back the ones Steam ate.

    tools/keep_manifests.py             # save new ones, restore missing ones
    tools/keep_manifests.py --dry-run
    tools/keep_manifests.py --steam ~/.steam/steam

Steam removes a depot's manifest from depotcache once that depot is installed.
For a game you own that costs nothing, because Steam asks Valve for it again.
With injected ownership it cannot: fetching a manifest needs a request code and
Valve issues those only to accounts that own the app. So the install succeeds,
and then the next update or verify fails with 'Access Denied', which the client
shows as "no internet connection".

The manifests that came with a pack are the only copies that exist on this
machine, and once Steam deletes one the pack has to be downloaded again to get
it back. So this keeps a copy somewhere Steam does not manage, and restores any
pinned manifest that has gone missing from depotcache.

Worth running before and after a download. Nothing here needs Steam closed:
depotcache is Steam's, but adding a file it is not currently reading is safe,
and the vault is ours alone.
"""
import argparse
import pathlib
import shutil
import sys
import re

PIN = re.compile(r'^[ \t]*setManifestid\(\s*(\d+)\s*,\s*"(\d+)"', re.M | re.I)


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
    depotcache = steam / "depotcache"
    vault = steam / "steamflipper" / "manifests"
    lua_dir = steam / "config" / "stplug-in"

    if not depotcache.is_dir():
        print(f"  no depotcache at {depotcache}; nothing to do")
        return 0
    if not args.dry_run:
        vault.mkdir(parents=True, exist_ok=True)

    # --- save whatever is there now, before Steam gets to it ---
    saved = 0
    for m in sorted(depotcache.glob("*.manifest")):
        target = vault / m.name
        if target.exists():
            continue
        saved += 1
        if not args.dry_run:
            shutil.copy2(m, target)

    # --- put back anything a pack still pins that depotcache has lost ---
    restored = 0
    unrecoverable = []
    if lua_dir.is_dir():
        for f in sorted(lua_dir.glob("*.lua")):
            for depot, gid in PIN.findall(f.read_text(errors="replace")):
                name = f"{depot}_{gid}.manifest"
                if (depotcache / name).is_file():
                    continue
                if (vault / name).is_file():
                    restored += 1
                    if not args.dry_run:
                        shutil.copy2(vault / name, depotcache / name)
                else:
                    # Never seen by this tool, so there is no copy to give back.
                    # Only a fresh download of the pack can supply it.
                    unrecoverable.append((f.name, name))

    print(f"  copied into the vault : {saved}")
    print(f"  restored to depotcache: {restored}")
    print(f"  gone, no copy held    : {len(unrecoverable)}")
    for lua, name in unrecoverable[:10]:
        print(f"      {lua:16} {name}")
    if len(unrecoverable) > 10:
        print(f"      ... and {len(unrecoverable) - 10} more")
    if unrecoverable:
        print("  those need the pack downloading again; the manifest only ever")
        print("  came with it, and Valve will not issue one for an app you do")
        print("  not own.")
    if args.dry_run:
        print("  dry run; nothing written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
