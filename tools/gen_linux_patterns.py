#!/usr/bin/env python3
"""Derive SteamFlipper pattern RVAs from a Linux steamclient.so.

Steam ships stripped binaries with no symbols for its internal functions, but it
builds them with VProf enabled: a profiled function opens a scope naming itself,
so the name string's only code reference lands inside that function, and the
.eh_frame FDE covering the reference gives the function's exact bounds.

Only functions carrying a VProf scope are recoverable this way. Everything else
GCC either never profiled or inlined away, which is why the output is small.

Steam rewrites steamclient.so on update and pattern files are keyed by SHA-256,
so re-run this after every client update:

    tools/gen_linux_patterns.py ~/.local/share/Steam/ubuntu12_32/steamclient.so --install
"""

import argparse
import bisect
import hashlib
import pathlib
import re
import struct
import subprocess
import sys

# Functions SteamFlipper resolves that carry a self-naming VProf scope, per
# component. Names are looked up by FNV-1a hash, so they must match the strings
# SteamFlipper passes to PatternLoader::FindPattern exactly.
# Maps the name SteamFlipper asks for -> the VProf scope strings to search for.
# They differ: SteamFlipper uses short aliases while Valve's scopes are usually
# qualified ("CSteamUIAppController::FillInAppOverview"). The FNV-1a key is
# always computed from the SteamFlipper name, never the scope string.
WANTED = {
    "steamclient": {
        "CheckAppOwnership":    ("CheckAppOwnership",),
        "BuildDepotDependency": ("BuildDepotDependency",),
        "GetOrAddAppData":      ("GetOrAddAppData",),
    },
    "steamui": {
        # FillInAppOverview is deliberately NOT listed. The VProf scope resolves
        # to 0x17F1FE0, which is a real and correctly-identified body -- but it
        # is unreachable: a full linear disassembly of the binary finds zero
        # call/jmp references to it, so a detour there could never fire. GCC
        # emitted a regparm clone that callers actually use. Emitting it gains
        # nothing (it only set a cosmetic PurchasedTime) and only adds a patched
        # site that has already been implicated in a client segfault.
        "BuildCompleteAppOverviewChange": ("BuildCompleteAppOverviewChange",),
        "GetAppByID":                     ("CSteamUIAppController::GetAppByID",),
        "MarkAppChange":                  ("CUpdateManager::MarkAppChange",),
        # Deliberately only the exact scope. steamui.so has no
        # CSteamUIAppController::RunFrame string, so this normally resolves to
        # nothing. Do NOT fall back to CVGUIApplicationSteamUI::RunFrame — it is
        # a different class with a different signature, and hooking it crashes
        # Steam on startup (verified). Leaving this unresolved only disables the
        # library-removal drain; it does not affect unlocking.
        "CSteamUIAppControllerRunFrame":  ("CSteamUIAppController::RunFrame",),
    },
}


# Addresses recovered by structural analysis rather than a VProf scope, and each
# independently re-derived by a second analyst before being written down. They
# CANNOT be re-derived by the generic path below, so they are pinned to the exact
# SHA-256 they were verified against: a Steam update changes every offset, and
# emitting a stale address makes funchook patch unrelated memory. If the hash
# does not match, these are skipped with a warning rather than guessed.
#
# Deliberately absent: FillInAppOverview. Its body at 0x17F1FE0 is real and
# correctly identified but UNREACHABLE — zero call/jmp references anywhere in the
# binary — so a detour there can never fire. It only sets PurchasedTime (cosmetic)
# and is not on the unlock path.
VERIFIED = {
    # steamclient.so
    "bc54101b290f9a5b4a0713c9084494a5f05097826f66a661dbe322c469b764a2": {
        "GetPackageInfo":               0xFE8850,
        "CUtlMemoryGrow":               0xFF4790,
        "MarkLicenseAsChanged":         0x188C700,
        "ProcessPendingLicenseUpdates": 0x188C950,
        # Not a function: the address of the global pointer to the object that
        # owns CPackageInfoCache. Decoded from Steam's own call site, which does
        #   lea eax,[GOT+0x3b7d4]; mov eax,[eax]; add eax,0xc40; call GetPackageInfo
        # so the cache is a subobject at +0xC40 of *(this global). Lets us obtain
        # `this` without patching GetPackageInfo (see Hooks_Package.cpp).
        "CPackageInfoCacheGlobal":      0x2F85B20,
    },
    # steamui.so
    "b3d2a355684ada5e34a4faba103324660ce479597d0b18638c9680cd0c798b18": {
        # The live cdecl body. GCC also emits regparm/constprop clones of
        # GetAppByID and FillInAppOverview; hooking a clone misreads arguments.
        "CSteamUIAppControllerRunFrame": 0x18005D0,
    },
}


def component_of(path):
    """steamclient.so -> 'steamclient'. Decides both the name list and the
    cache subdirectory PatternLoader reads."""
    stem = pathlib.Path(path).name
    for known in WANTED:
        if stem.startswith(known):
            return known
    sys.exit(f"unrecognised binary '{stem}'; expected steamclient.so or steamui.so")


def fnv1a(s: str) -> int:
    h = 2166136261
    for b in s.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def sections(path):
    """[(name, addr, offset, size)] from readelf, for file-offset <-> vaddr mapping.

    SHT_NOBITS sections (.bss, .tbss) are excluded. They carry a nominal file
    offset but occupy no file bytes, so leaving them in makes their phantom
    range shadow whatever really lives at those offsets -- turning a data
    address lookup into a wrong answer rather than a miss.
    """
    out = subprocess.run(["readelf", "-SW", str(path)],
                         capture_output=True, text=True).stdout
    secs = []
    for m in re.finditer(
            r"\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out):
        name, stype = m.group(1), m.group(2)
        addr, off, size = (int(g, 16) for g in m.groups()[2:])
        if stype == "NOBITS":
            continue
        secs.append((name, addr, off, size))
    if not secs:
        sys.exit("readelf produced no sections")
    return secs


def off_to_vaddr(secs, off):
    for _, addr, o, size in secs:
        if addr and o <= off < o + size:
            return addr + (off - o)
    return None


def vaddr_of_string(data, secs, name):
    """File offset and vaddr of a NUL-terminated string, searched in rodata-ish sections."""
    needle = b"\x00" + name.encode() + b"\x00"
    i = data.find(needle)
    if i < 0:
        return None, None
    off = i + 1
    return off, off_to_vaddr(secs, off)


def got_address(data, secs):
    """Recover the GOT base empirically from PIC prologues.

    i386 PIC sets up its base as `call __x86.get_pc_thunk.R; add R, <imm32>`,
    where the thunk returns the address of the instruction after the call. So
    GOT = (address after the call) + imm32. Every PIC function agrees on the
    value, so take the most common one — that survives the GOT moving between
    Steam builds, which hardcoding a section address does not.
    """
    from collections import Counter
    votes = Counter()
    for m in re.finditer(rb"\xe8...(?:.)(\x05|\x81[\xc0-\xc7])", data, re.DOTALL):
        call_off = m.start()
        after = call_off + 5                       # instruction following the call
        add_off = m.start(1)
        if add_off != after:
            continue
        imm_off = after + (1 if data[add_off] == 0x05 else 2)
        if imm_off + 4 > len(data):
            continue
        imm = struct.unpack_from("<i", data, imm_off)[0]
        va = off_to_vaddr(secs, after)
        if va is not None:
            votes[(va + imm) & 0xFFFFFFFF] += 1
    if not votes:
        sys.exit("could not derive GOT base from PIC prologues")
    got, n = votes.most_common(1)[0]
    print(f"  GOT base 0x{got:X} ({n} agreeing prologues)", file=sys.stderr)
    return got


def fdes(path):
    """Sorted [(lo, hi)] function bounds from .eh_frame FDEs."""
    out = subprocess.run(["readelf", "--debug-dump=frames-interp", str(path)],
                         capture_output=True, text=True).stdout
    res = []
    for m in re.finditer(r"FDE .*?pc=([0-9a-f]+)\.\.([0-9a-f]+)", out):
        res.append((int(m.group(1), 16), int(m.group(2), 16)))
    if not res:
        sys.exit("no FDEs found in .eh_frame")
    res.sort()
    return res


def make_sig(data, secs, vaddr, n=24):
    """Prologue bytes, wildcarding PIC-thunk calls and GOT-delta immediates."""
    off = None
    for _, addr, o, size in secs:
        if addr and addr <= vaddr < addr + size:
            off = o + (vaddr - addr)
            break
    if off is None:
        return ""
    b = data[off:off + n]
    out, i = [], 0
    while i < len(b):
        op = b[i]
        if op == 0xE8 and i + 5 <= len(b):                    # call rel32 (PIC thunk)
            out += ["E8", "??", "??", "??", "??"]; i += 5
        elif op == 0x05 and i + 5 <= len(b):                  # add eax, imm32 (GOT)
            out += ["05", "??", "??", "??", "??"]; i += 5
        elif op == 0x81 and i + 6 <= len(b) and (b[i + 1] & 0xF8) == 0xC0:
            out += ["81", f"{b[i+1]:02X}", "??", "??", "??", "??"]; i += 6
        else:
            out.append(f"{op:02X}"); i += 1
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", type=pathlib.Path)
    ap.add_argument("--partial", action="store_true",
                    help="install a VProf-only pattern file for an uncalibrated "
                         "Steam build (ownership unlock will not work)")
    ap.add_argument("--allow-steamui", action="store_true",
                    help="emit steamui.so patterns; they crash the client (see below)")
    ap.add_argument("--install", action="store_true",
                    help="write into <binary dir>/steamflipper/pattern/<component>/")
    args = ap.parse_args()

    component = component_of(args.binary)
    if component == "steamui" and not args.allow_steamui:
        sys.exit(
            "refusing to generate steamui.so patterns.\n"
            "  Installing them segfaults the Steam client on startup, reproduced\n"
            "  across several address sets including independently verified ones.\n"
            "  They only drive library cosmetics and app-removal, not unlocking,\n"
            "  so nothing is lost by leaving them off.\n"
            "  Pass --allow-steamui if you are actively debugging this.")
    data = args.binary.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    secs = sections(args.binary)
    got = got_address(data, secs)
    table = fdes(args.binary)
    starts = [f[0] for f in table]

    def fde_for(a):
        i = bisect.bisect_right(starts, a) - 1
        return table[i] if i >= 0 and table[i][0] <= a < table[i][1] else None

    found = {}
    for name, scopes in WANTED[component].items():
        hit, used = None, None
        for scope in scopes:
            matches = []
            _, sv = vaddr_of_string(data, secs, scope)
            if sv is None:
                continue
            # PIC code references the string as lea reg,[ebx + (str - GOT)].
            needle = struct.pack("<i", sv - got)
            start = 0
            while True:
                i = data.find(needle, start)
                if i < 0:
                    break
                start = i + 1
                va = off_to_vaddr(secs, i)
                f = fde_for(va) if va else None
                if f and f not in matches:
                    matches.append(f)
            if len(matches) > 1:
                # GCC emits regparm/constprop clones; the scope string then
                # lands in several bodies and "first match" is a coin flip.
                # Hooking the wrong one crashes the client, so refuse.
                print(f"  {name:<31} AMBIGUOUS: {len(matches)} functions reference "
                      f"'{scope}' ({', '.join(hex(m[0]) for m in matches)}); "
                      f"skipping rather than guessing", file=sys.stderr)
                matches = []
                continue
            if matches:
                hit, used = matches[0], scope
                break
        if hit:
            found[name] = hit[0]
            print(f"  {name:<31} -> 0x{hit[0]:X}  ({hit[1]-hit[0]} bytes)  via '{used}'",
                  file=sys.stderr)
        else:
            print(f"  {name:<31} NOT FOUND (scopes tried: {', '.join(scopes)})", file=sys.stderr)

    # Merge in structurally-verified addresses, but only for the exact binary
    # they were confirmed against.
    pinned = VERIFIED.get(sha)
    if pinned:
        for name, rva in pinned.items():
            if fde_for(rva) and fde_for(rva)[0] != rva:
                print(f"  {name:<31} REJECTED: 0x{rva:X} is not an FDE start", file=sys.stderr)
                continue
            found[name] = rva
            print(f"  {name:<31} -> 0x{rva:X}  (verified, pinned to this sha256)", file=sys.stderr)
    else:
        print(f"  NO VERIFIED TABLE for sha {sha[:16]}... — this is a Steam build\n"
              f"  this generator has not been calibrated against. The pinned,\n"
              f"  structurally-derived addresses (GetPackageInfo, CUtlMemoryGrow,\n"
              f"  MarkLicenseAsChanged, ProcessPendingLicenseUpdates,\n"
              f"  CPackageInfoCacheGlobal) cannot be re-derived automatically and\n"
              f"  are NOT included.\n"
              f"  Installing the VProf-only subset would give a partial hook set:\n"
              f"  ownership injection silently does nothing while everything else\n"
              f"  looks fine. Refusing.\n"
              f"  Pass --partial to install anyway (ownership unlock will not work).",
              file=sys.stderr)
        if not args.partial:
            return 1

    if not found:
        sys.exit("nothing resolved; VProf strings may be absent from this build")

    lines = [
        f"# {args.binary.name} - Linux 32-bit, generated by tools/gen_linux_patterns.py",
        f"# sha256 = {sha}",
        "#",
        "# Derived from Valve's VProf scopes (see the script for the method).",
        "# Only VProf-scoped functions are recoverable; the rest were inlined by GCC.",
        "# rva takes priority over sig in PatternLoader; sigs are a fallback and",
        "# wildcard the PIC thunk call and GOT-delta immediates.",
        "",
    ]
    for name in sorted(found):
        va = found[name]
        lines += [f"[0x{fnv1a(name):08X}]",
                  f'name = "{name}"',
                  f'rva = "0x{va:X}"',
                  f'sig = "{make_sig(data, secs, va)}"',
                  ""]
    text = "\n".join(lines)

    if args.install:
        dest = args.binary.parent / "steamflipper" / "pattern" / component
        dest.mkdir(parents=True, exist_ok=True)
        out = dest / f"{sha}.toml"
        out.write_text(text)
        print(f"  installed -> {out}", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
