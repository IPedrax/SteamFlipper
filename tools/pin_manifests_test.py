#!/usr/bin/env python3
"""Runnable check for tools/pin_manifests.py.

    ./tools/pin_manifests_test.py

Runs the real script against a throwaway Steam directory, so the edits land on
fixtures rather than on whoever runs this.

Worth having because this rewrites files the user did not create, and both ways
of being wrong are quiet. Uncomment a pin whose manifest is not on disk and the
download fails exactly as it did before, with the cause now hidden behind an
edit. Fail to uncomment one that is there and the game stays broken for the
reason this exists to fix. The loader is case-insensitive about these calls, so
the pattern has to be too.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SUBJECT = HERE / "pin_manifests.py"

failures = 0


def check(ok, what):
    global failures
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures += 1


def make_steam(root: pathlib.Path, lua: str, manifests) -> pathlib.Path:
    (root / "config" / "stplug-in").mkdir(parents=True)
    (root / "depotcache").mkdir()
    (root / "config" / "config.vdf").write_text('"InstallConfigStore"\n{\n}\n')
    (root / "config" / "stplug-in" / "999.lua").write_text(lua)
    for name in manifests:
        (root / "depotcache" / name).write_text("x")
    return root


def run(root, *extra):
    return subprocess.run(
        [sys.executable, str(SUBJECT), "--steam", str(root), *extra],
        capture_output=True, text=True)


LUA = '''-- a pack, as they ship
addappid(999)
addappid(1000, 1, "{k}")
-- setManifestid(1000, "111", 4117)
	-- setManifestid(1001, "222")
--setManifestid(1002, "333")
setManifestid(1003, "444")
-- SETMANIFESTID(1004, "555")
'''.replace("{k}", "a" * 64)

tmp = pathlib.Path(tempfile.mkdtemp())
try:
    print("A pin whose manifest is on disk")
    root = make_steam(tmp / "a", LUA,
                      ["1000_111.manifest", "1001_222.manifest", "1004_555.manifest"])
    out = run(root).stdout
    text = (root / "config" / "stplug-in" / "999.lua").read_text()

    check('setManifestid(1000, "111", 4117)' in text and
          '-- setManifestid(1000, "111", 4117)' not in text,
          "is uncommented, keeping the whole call including the size argument")
    check("\t" + 'setManifestid(1001, "222")' in text,
          "and its original indentation survives")
    check('SETMANIFESTID(1004, "555")' in text and
          '-- SETMANIFESTID(1004, "555")' not in text,
          "a case variant is handled, because the loader accepts any")

    print("A pin whose manifest is absent")
    check('--setManifestid(1002, "333")' in text,
          "stays commented, since pinning it would fail the same way")

    print("A pin that was already active")
    check(text.count('setManifestid(1003, "444")') == 1 and
          '-- setManifestid(1003, "444")' not in text,
          "is left exactly as it was")

    print("The rest of the file")
    check('addappid(1000, 1, "' + "a" * 64 + '")' in text, "keys are untouched")
    check(text.startswith("-- a pack, as they ship"),
          "and an ordinary comment is not mistaken for a pin")

    print("The backup")
    orig = root / "config" / "stplug-in" / "999.lua.sf-orig"
    check(orig.is_file(), "the original is kept beside the file")
    check('-- setManifestid(1000, "111", 4117)' in orig.read_text(),
          "and still has the pins commented, so the pack can be restored")

    print("Running it twice")
    before = text
    run(root)
    check((root / "config" / "stplug-in" / "999.lua").read_text() == before,
          "changes nothing the second time")

    print("A dry run")
    root2 = make_steam(tmp / "b", LUA, ["1000_111.manifest"])
    out2 = run(root2, "--dry-run").stdout
    check("nothing written" in out2, "says so")
    check((root2 / "config" / "stplug-in" / "999.lua").read_text() == LUA,
          "and leaves the file byte for byte as it was")
    check(not (root2 / "config" / "stplug-in" / "999.lua.sf-orig").exists(),
          "writing no backup either")

    print("A Steam with no manifests yet")
    root3 = tmp / "c"
    (root3 / "config").mkdir(parents=True)
    (root3 / "config" / "config.vdf").write_text("{}")
    r = run(root3)
    check(r.returncode == 0, "exits cleanly rather than treating it as an error")
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("\nall passed" if not failures else f"\n{failures} FAILED")
sys.exit(1 if failures else 0)
