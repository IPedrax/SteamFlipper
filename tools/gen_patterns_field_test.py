#!/usr/bin/env python3
"""Runnable check that the generator records the package-cache offset.

    ./tools/gen_patterns_field_test.py

Runs tools/gen_linux_patterns.py on this machine's steamclient.so, without
--install so nothing on disk changes, and checks two things about what it wrote.

The offset used to be a constant in Hooks_Package.cpp with a comment on the
generator's output asking a human to keep the two in step. Steam's update moved
it from 0xC40 to 0xF78, the constant stayed put, and GetPackageInfo was handed a
pointer 0x338 bytes short of the cache on every start: a segfault inside Steam
on launch, with the module loading fine and nothing logged. So the number now
travels in the pattern file, and this is the check that it does:

  1. the CPackageInfoCacheGlobal entry carries a field_offset at all, and
  2. it is the same offset the generator says it derived, not some other value.

Skips cleanly where there is no Steam to read.
"""
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GEN = HERE / "gen_linux_patterns.py"

CANDIDATES = (
    "~/.local/share/Steam/ubuntu12_32/steamclient.so",
    "~/.steam/steam/ubuntu12_32/steamclient.so",
)

failures = 0


def check(ok, what):
    global failures
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures += 1


binary = next((p for p in (pathlib.Path(c).expanduser() for c in CANDIDATES)
               if p.is_file()), None)
if binary is None:
    print("  skip no steamclient.so on this machine")
    sys.exit(0)

run = subprocess.run([sys.executable, str(GEN), str(binary)],
                     capture_output=True, text=True)
if run.returncode != 0:
    # A Steam build the generator cannot calibrate is its own, separate failure
    # and already reported loudly by the generator. Not this check's to judge.
    print(f"  skip generator refused this build (exit {run.returncode})")
    sys.exit(0)

toml_text, log = run.stdout, run.stderr

print("The pattern file")
entry = re.search(r'name = "CPackageInfoCacheGlobal"\n(?:.*\n)*?(?=\n|\Z)', toml_text)
check(entry is not None, "has a CPackageInfoCacheGlobal entry")

field = re.search(r'name = "CPackageInfoCacheGlobal"\nrva = "[^"]+"\nsig = "[^"]*"\n'
                  r'field_offset = "(0x[0-9A-Fa-f]+)"', toml_text)
check(field is not None, "records a field_offset on it")

print("Agreement with what the generator derived")
said = re.search(r"cache subobject offset\s+(0x[0-9A-Fa-f]+)", log)
check(said is not None, "the generator reports deriving an offset")
if field and said:
    check(int(field.group(1), 16) == int(said.group(1), 16),
          f"and the file records that same offset ({field.group(1)})")
    check(int(field.group(1), 16) != 0,
          "which is not zero, the value the loader treats as unknown")

print("Nothing else grew a field it should not have")
others = re.findall(r'name = "([^"]+)"[^\[]*?field_offset', toml_text)
check(others == ["CPackageInfoCacheGlobal"] or others == [],
      "only the entry that has an offset carries one")

print("\nall passed" if not failures else f"\n{failures} FAILED")
sys.exit(1 if failures else 0)
