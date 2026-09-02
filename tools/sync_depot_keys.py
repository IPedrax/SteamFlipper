#!/usr/bin/env python3
"""Copy depot decryption keys out of Lua manifests into Steam's config.vdf.

A Lua manifest carries its depot keys as `addappid(<depot>, 1, "<64 hex>")`.
SteamFlipper can feed those to Steam by hooking ConfigStoreGetBinary, but that
hook needs a byte-pattern for a function Valve does not give a VProf scope, and a
wrong address there crashes the client.

Steam already persists the same keys in `config/config.vdf` under `depots`, and
reads them natively. Writing them there needs no hook, no signature, and survives
Steam updates that invalidate every RVA -- so it is the preferred route on Linux.
Without the keys, owned content downloads but fails with "Missing decryption key".

    tools/sync_depot_keys.py            # default Steam dir, writes changes
    tools/sync_depot_keys.py --dry-run  # report what would change
    tools/sync_depot_keys.py --steam ~/.steam/steam

Steam must be closed: it rewrites config.vdf on exit and would discard edits made
underneath it.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

# addappid(<depot>, 1, "<64 hex chars>") -- the trailing 1 means "has a key".
KEY_RE = re.compile(r'^\s*addappid\(\s*(\d+)\s*,\s*1\s*,\s*"([0-9a-fA-F]{64})"', re.M)

# A depot entry already present in config.vdf's depots block.
EXISTING_RE = re.compile(r'"(\d+)"\s*\{\s*"DecryptionKey"')

# The depots block header and its opening brace, capturing the indent style so
# inserted entries match the surrounding file rather than introducing a new one.
DEPOTS_RE = re.compile(r'([\t ]*)"depots"\s*\r?\n([\t ]*)\{')


def default_steam_dir() -> pathlib.Path:
    for candidate in ("~/.local/share/Steam", "~/.steam/steam", "~/.steam/root"):
        p = pathlib.Path(candidate).expanduser()
        if (p / "config" / "config.vdf").is_file():
            return p
    sys.exit("could not find a Steam directory; pass --steam")


def steam_is_running() -> bool:
    try:
        return subprocess.run(["pgrep", "-x", "steam"],
                              capture_output=True).returncode == 0
    except FileNotFoundError:
        return False


def collect_keys(lua_dir: pathlib.Path) -> dict:
    """depot id -> key, first definition wins (matches SteamFlipper's own order)."""
    keys = {}
    for f in sorted(lua_dir.glob("*.lua")):
        try:
            text = f.read_bytes().decode("utf-8", errors="surrogateescape")
        except OSError as e:
            print(f"  ! unreadable, skipped: {f.name} ({e})", file=sys.stderr)
            continue
        for depot, key in KEY_RE.findall(text):
            keys.setdefault(depot, key.lower())
    return keys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--steam", type=pathlib.Path, default=None,
                    help="Steam directory (default: autodetect)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change without writing")
    args = ap.parse_args()

    steam = args.steam.expanduser() if args.steam else default_steam_dir()
    lua_dir = steam / "config" / "stplug-in"
    cfg_path = steam / "config" / "config.vdf"

    if not cfg_path.is_file():
        sys.exit(f"no config.vdf under {steam}")
    if not lua_dir.is_dir():
        # Normal on a fresh install: the user has not added any manifests yet.
        # Exiting non-zero here made every first-time install end on a scary
        # "depot key sync failed" warning.
        print(f"  no Lua manifest directory yet ({lua_dir}) — nothing to sync")
        return 0

    keys = collect_keys(lua_dir)
    # Byte-exact round-trip: errors="replace" would rewrite any byte that is
    # invalid in the locale encoding as U+FFFD and silently corrupt unrelated
    # fields (account names, install paths) on write-back.
    cfg = cfg_path.read_bytes().decode("utf-8", errors="surrogateescape")
    present = set(EXISTING_RE.findall(cfg))
    new = {d: k for d, k in keys.items() if d not in present}

    print(f"  Lua manifests      : {len(list(lua_dir.glob('*.lua')))}")
    print(f"  depot keys in Luas : {len(keys)}")
    print(f"  already in config  : {len(present)}")
    print(f"  to add             : {len(new)}")

    if not new:
        print("  nothing to do")
        return 0
    if args.dry_run:
        for d in sorted(new, key=int)[:20]:
            print(f"    + depot {d}")
        if len(new) > 20:
            print(f"    ... and {len(new) - 20} more")
        return 0

    # Steam rewrites config.vdf on exit; editing it underneath a running client
    # silently loses everything written here.
    if steam_is_running():
        sys.exit("Steam is running -- close it first (steam -shutdown), then re-run")

    m = DEPOTS_RE.search(cfg)
    if not m:
        sys.exit('no "depots" block in config.vdf; log in and install one game '
                 'first so Steam creates it')

    entry_ind = m.group(2) + "\t"
    key_ind = entry_ind + "\t"
    block = "".join(
        f'\n{entry_ind}"{d}"\n{entry_ind}{{\n{key_ind}"DecryptionKey"\t\t"{new[d]}"\n{entry_ind}}}'
        for d in sorted(new, key=int))
    out = cfg[:m.end()] + block + cfg[m.end():]

    # Structural check before overwriting: one extra {} pair per inserted depot.
    if out.count("{") != cfg.count("{") + len(new) or out.count("}") != cfg.count("}") + len(new):
        sys.exit("refusing to write: brace count did not change as expected")

    backup = cfg_path.with_suffix(f".vdf.bst-{time.strftime('%Y%m%d-%H%M%S')}.bak")
    shutil.copy2(cfg_path, backup)
    # Write via a temp file in the same directory, then rename, so an interrupted
    # run cannot leave a half-written config.vdf behind.
    tmp = cfg_path.with_suffix(".vdf.bst-tmp")
    tmp.write_bytes(out.encode("utf-8", errors="surrogateescape"))
    os.replace(tmp, cfg_path)

    print(f"  backup             : {backup.name}")
    print(f"  added {len(new)} depot keys -> {cfg_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
