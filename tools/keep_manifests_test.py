#!/usr/bin/env python3
"""Runnable check for tools/keep_manifests.py.

    ./tools/keep_manifests_test.py

Runs the real script against a throwaway Steam directory.

Worth having because both halves fail quietly. Miss a manifest on the way in
and the only copy on the machine is gone the next time Steam installs that
depot, recoverable only by downloading the pack again. Restore one that was
never pinned, or one a pack deliberately left commented, and depotcache fills
with versions Steam was not asking for.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SUBJECT = HERE / "keep_manifests.py"

failures = 0


def check(ok, what):
    global failures
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures += 1


def make(root, lua, in_cache=(), in_vault=()):
    (root / "config" / "stplug-in").mkdir(parents=True)
    (root / "depotcache").mkdir()
    (root / "config" / "config.vdf").write_text("{}")
    (root / "config" / "stplug-in" / "77.lua").write_text(lua)
    for n in in_cache:
        (root / "depotcache" / n).write_text("cache")
    if in_vault:
        v = root / "steamflipper" / "manifests"
        v.mkdir(parents=True)
        for n in in_vault:
            (v / n).write_text("vault")
    return root


def run(root, *extra):
    return subprocess.run([sys.executable, str(SUBJECT), "--steam", str(root), *extra],
                          capture_output=True, text=True)


LUA = '''addappid(77)
setManifestid(100, "111")
setManifestid(200, "222")
-- setManifestid(300, "333")
'''

tmp = pathlib.Path(tempfile.mkdtemp())
try:
    print("Saving what is there")
    root = make(tmp / "a", LUA, in_cache=["100_111.manifest", "999_888.manifest"])
    run(root)
    vault = root / "steamflipper" / "manifests"
    check((vault / "100_111.manifest").is_file(), "a pinned manifest is copied in")
    check((vault / "999_888.manifest").is_file(),
          "and so is one nothing pins, because a pack may pin it later")

    print("Restoring one Steam removed")
    # 200_222 is pinned, absent from depotcache, and held in the vault: exactly
    # the state after Steam consumes a manifest during an install.
    root = make(tmp / "b", LUA, in_cache=[], in_vault=["200_222.manifest"])
    out = run(root).stdout
    dc = root / "depotcache" / "200_222.manifest"
    check(dc.is_file(), "it is put back into depotcache")
    check(dc.read_text() == "vault", "from the vault copy, not invented")
    check("restored to depotcache: 1" in out, "and the count says so")

    print("A pin with no copy anywhere")
    check("gone, no copy held    : 1" in out,
          "is reported rather than passed over")
    check("100_111.manifest" in out, "by name, so the pack can be fetched again")

    print("A pin the pack left commented")
    check(not (root / "depotcache" / "300_333.manifest").exists(),
          "is not restored, because Steam is not asking for that version")
    check("300_333" not in out, "and is not reported as missing either")

    print("Running it twice")
    root = make(tmp / "c", LUA, in_cache=["100_111.manifest"])
    run(root)
    mtime = (root / "steamflipper" / "manifests" / "100_111.manifest").stat().st_mtime_ns
    out2 = run(root).stdout
    check("copied into the vault : 0" in out2, "copies nothing the second time")
    check((root / "steamflipper" / "manifests" / "100_111.manifest").stat().st_mtime_ns == mtime,
          "leaving the held copy untouched")

    print("A dry run")
    root = make(tmp / "d", LUA, in_cache=["100_111.manifest"], in_vault=["200_222.manifest"])
    out3 = run(root, "--dry-run").stdout
    check("nothing written" in out3, "says so")
    check(not (root / "depotcache" / "200_222.manifest").exists(),
          "restores nothing")
    check(not (root / "steamflipper" / "manifests" / "100_111.manifest").exists(),
          "and saves nothing")

    print("A Steam with no depotcache")
    root = tmp / "e"
    (root / "config").mkdir(parents=True)
    (root / "config" / "config.vdf").write_text("{}")
    r = run(root)
    check(r.returncode == 0, "exits cleanly instead of failing")
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("\nall passed" if not failures else f"\n{failures} FAILED")
sys.exit(1 if failures else 0)
