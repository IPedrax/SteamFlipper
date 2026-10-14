#!/usr/bin/env python3
"""Runnable check that the generator records per-build offsets, and records only
ones it is sure of.

    ./tools/gen_patterns_field_test.py

Steam's update moved two internal structures that this module had compiled in as
constants. The package cache went from 0xC40 to 0xF78 and GetPackageInfo was
handed a pointer 0x338 bytes short of it: a segfault inside Steam on every start.
The config store went from 0xD60 to 0x1098 and its lookup found no vtable. Both
used to be numbers in the module with a comment asking a human to keep them in
step with the generator, so both are recorded in the pattern file now.

Two halves. The first runs the real derivation against built bytes, because the
reason to be strict is that hooking a function picked from a near miss crashes
the client: a wrong global, offsets that disagree, or two different answers must
each produce nothing. The second runs the real generator on this machine's
steamclient.so and checks what it wrote is what it said it derived.
"""
import importlib.util
import pathlib
import re
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GEN = HERE / "gen_linux_patterns.py"

spec = importlib.util.spec_from_file_location("gen", GEN)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

failures = 0


def check(ok, what):
    global failures
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures += 1


# --- built bytes ------------------------------------------------------------
GOT = 0x01000000
STR_VA = 0x00200000
GLOBAL_VA = GOT + 0x3C724          # where the singleton lives
# offset == vaddr, as in the real file. The address must be non-zero: the
# generator treats a section at address 0 as unmapped, which an earlier draft of
# this fixture tripped on, and every refusal below then "passed" for want of
# anything to match. The positive cases are why that cannot go unnoticed.
SECS = [(".rodata", 0x1000, 0x1000, 1 << 30)]


def ref_lea():
    # lea -(GOT-STR)(%ebx),%eax : how the format string's address is formed
    return b"\x8d\x83" + struct.pack("<i", STR_VA - GOT)


def seq(global_va=GLOBAL_VA, off_a=0x1098, off_b=None, slot=0x18):
    """The read site's load sequence, in the shape both builds share."""
    off_b = off_a if off_b is None else off_b
    return (b"\x8d\x83" + struct.pack("<i", global_va - GOT) +   # lea global
            b"\x8b\x00" +                                         # mov (%eax),%eax
            b"\x8d\x90" + struct.pack("<I", off_a) +              # lea OFF(%eax),%edx
            b"\x8b\x80" + struct.pack("<I", off_b) +              # mov OFF(%eax),%eax
            b"\x8b\x40" + bytes([slot]))                          # mov slot(%eax),%eax


def image(*sites):
    """A fake binary: the format string, then each call site after padding."""
    data = bytearray(b"\x90" * 0x400000)
    s = b"\x00" + gen.CONFIG_KEY_FORMAT.encode() + b"\x00"
    data[STR_VA - 1: STR_VA - 1 + len(s)] = s
    pos = 0x300000
    for site in sites:
        blob = ref_lea() + b"\x83\xc4\x0c" + b"\x90" * 8 + site
        data[pos: pos + len(blob)] = blob
        pos += 0x1000
    return bytes(data)


def derive(*sites):
    return gen.derive_config_store(image(*sites), SECS, GOT, GLOBAL_VA)


print("A call site shaped like both builds seen")
check(derive(seq(off_a=0x1098)) == (0x1098, 6), "the current build reads as (0x1098, slot 6)")
check(derive(seq(off_a=0xD60)) == (0xD60, 6), "and the older build as (0xD60, slot 6)")

print("Near misses that must produce nothing")
check(derive(seq(global_va=GLOBAL_VA + 8)) is None,
      "a different global is refused: it is not the singleton the module resolves")
check(derive(seq(off_a=0x1098, off_b=0x10A0)) is None,
      "offsets that disagree are refused, since that is not one subobject")
check(derive(seq(slot=0x1A)) is None,
      "a displacement that is not a whole vtable slot is refused")
check(derive(b"\x90" * 40) is None, "a string with no load sequence after it gives nothing")
check(gen.derive_config_store(b"\x90" * 4096, SECS, GOT, GLOBAL_VA) is None,
      "and a binary without the string gives nothing")

print("Two different answers")
check(derive(seq(off_a=0x1098, slot=0x18), seq(off_a=0x1098, slot=0x38)) is None,
      "the read site and a write site that both match is refused, not guessed between")
check(derive(seq(off_a=0x1098), seq(off_a=0x1098)) == (0x1098, 6),
      "while the same answer from two sites is fine")

# --- the real generator -----------------------------------------------------
CANDIDATES = (
    "~/.local/share/Steam/ubuntu12_32/steamclient.so",
    "~/.steam/steam/ubuntu12_32/steamclient.so",
)
binary = next((p for p in (pathlib.Path(c).expanduser() for c in CANDIDATES)
               if p.is_file()), None)

print("This machine's steamclient.so")
if binary is None:
    print("  skip no steamclient.so on this machine")
else:
    run = subprocess.run([sys.executable, str(GEN), str(binary)],
                         capture_output=True, text=True)
    if run.returncode != 0:
        print(f"  skip generator refused this build (exit {run.returncode})")
    else:
        out, log = run.stdout, run.stderr
        entry = re.search(r'name = "CPackageInfoCacheGlobal"\n((?:[a-z_]+ = ".*"\n)+)', out)
        keys = dict(re.findall(r'([a-z_]+) = "(0x[0-9A-Fa-f]+)"',
                               entry.group(1))) if entry else {}

        said = re.search(r"cache subobject offset\s+(0x[0-9A-Fa-f]+)", log)
        check("field_offset" in keys and said is not None and
              int(keys["field_offset"], 16) == int(said.group(1), 16),
              f"the cache offset recorded is the one derived ({keys.get('field_offset')})")
        check(int(keys.get("field_offset", "0x0"), 16) != 0,
              "and it is not zero, the value the loader reads as unknown")

        said = re.search(r"config store subobject\s+(0x[0-9A-Fa-f]+), GetBinary is vtable slot (\d+)", log)
        if said:
            check(int(keys.get("config_store_offset", "0x0"), 16) == int(said.group(1), 16),
                  f"the config store offset recorded is the one derived ({keys.get('config_store_offset')})")
            check(int(keys.get("get_binary_slot", "0x0"), 16) == int(said.group(2)),
                  f"and so is the vtable slot ({said.group(2)})")
        else:
            check("config_store_offset" not in keys,
                  "a build it could not read records no config store offset at all")

        others = re.findall(r'name = "([^"]+)"\n((?:[a-z_]+ = ".*"\n)+)', out)
        stray = [n for n, body in others if n != "CPackageInfoCacheGlobal"
                 and re.search(r"^(field_offset|config_store_offset|get_binary_slot) =", body, re.M)]
        check(not stray, "and no other entry grew one of those fields")

print("\nall passed" if not failures else f"\n{failures} FAILED")
sys.exit(1 if failures else 0)
