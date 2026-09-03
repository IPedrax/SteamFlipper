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
        # GetPackageInfo, CPackageInfoCacheGlobal and CUtlMemoryGrow are now
        # derived, so they are no longer pinned here.
        "MarkLicenseAsChanged":         0x188C700,
        "ProcessPendingLicenseUpdates": 0x188C950,
    },
    # steamui.so
    "b3d2a355684ada5e34a4faba103324660ce479597d0b18638c9680cd0c798b18": {
        # The live cdecl body. GCC also emits regparm/constprop clones of
        # GetAppByID and FillInAppOverview; hooking a clone misreads arguments.
        "CSteamUIAppControllerRunFrame": 0x18005D0,
    },
}


# The full set each component needs, regardless of how it is obtained (VProf
# scope, derived from a call site, or pinned). Used to report the real gap on an
# uncalibrated Steam build -- WANTED alone lists only the VProf-derivable names,
# so intersecting against it hid the very entries that matter.
REQUIRED = {
    "steamclient": ("CheckAppOwnership", "BuildDepotDependency", "GetOrAddAppData",
                    "GetPackageInfo", "CUtlMemoryGrow", "MarkLicenseAsChanged",
                    "ProcessPendingLicenseUpdates", "CPackageInfoCacheGlobal"),
    "steamui": ("BuildCompleteAppOverviewChange", "GetAppByID", "MarkAppChange",
                "CSteamUIAppControllerRunFrame"),
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




# CUtlMemory<T>::Grow prologue. Steam instantiates this template hundreds of
# times, so the byte pattern alone is worthless as an identifier (569 hits).
_GROW_PROLOGUE = re.compile(
    rb"\x55\x57\x56\x53\xe8....\x81\xc3....\x83\xec\x0c"
    rb"\x8b\x7c\x24\x24\x8b\x74\x24\x20\x85\xff\x0f\x8e", re.S)


def _mask_pic(buf):
    """Blank PIC-relative immediates so two instantiations of one template
    compare equal: call rel32, add ebx/imm32, and lea reg,[reg+disp32]."""
    b = bytearray(buf)
    for m in re.finditer(rb"\xe8....|\x81\xc3....|\x8d[\x80-\xbf]....", bytes(buf), re.S):
        for i in range(m.start() + (1 if buf[m.start()] == 0xE8 else 2), m.end()):
            b[i] = 0
    return bytes(b)


def derive_cutlmemory_grow(data, secs, fde_for):
    """Locate a CUtlMemory<uint32>::Grow.

    OpenSteamTool only needs *a* Grow that handles 4-byte elements, because it
    calls it on PackageInfo::AppIdVec (CUtlVector<AppId_t>). The template is
    emitted many times over, and every 4-byte instantiation is the same code:
    verified by masking PIC immediates (243 byte-identical bodies) and by
    resolving their call targets, which match exactly. So any member of the
    largest such equivalence class is usable.

    Element size comes from the `shl eax,0x2` the body uses to scale the count,
    which is what distinguishes a 4-byte instantiation from the rest.
    """
    def to_off(va):
        for _, addr, off, size in secs:
            if addr and addr <= va < addr + size:
                return off + (va - addr)
        return None

    groups = {}
    for name, addr, off, size in secs:
        if name != ".text":
            continue
        for m in _GROW_PROLOGUE.finditer(data[off:off + size]):
            va = addr + m.start()
            f = fde_for(va)
            if not f or f[0] != va:
                continue                      # must be a real entry point
            o = to_off(va)
            body = data[o:o + (f[1] - f[0])]
            if b"\xc1\xe0\x02" not in body:  # shl eax,2 -> 4-byte elements
                continue
            groups.setdefault(_mask_pic(body), []).append(va)

    if not groups:
        return None, 0
    shape, addrs = max(groups.items(), key=lambda kv: len(kv[1]))
    if len(addrs) < 8:
        # A handful of look-alikes is not the mass-instantiated template; the
        # confidence this derivation rests on is gone, so do not guess.
        return None, len(addrs)
    return min(addrs), len(addrs)


def derive_from_call_site(data, secs, got, fde_for, anchor_lo, anchor_hi):
    """Recover GetPackageInfo and the CPackageInfoCache owner global by reading
    the call CheckAppOwnership already makes.

    CheckAppOwnership is VProf-derivable, and inside it Steam does exactly this:

        lea  reg,[GOT + D]        ; &g_pCPackageInfoOwner
        mov  [ebp - S],reg        ; stash it in a frame slot
        ...
        mov  eax,[ebp - S]        ; read the slot back
        mov  eax,[eax]            ; deref -> the owner object
        add  eax,<CACHE_OFF>      ; the cache is a subobject
        push eax                  ; this
        call <GetPackageInfo>

    Both addresses and the subobject offset fall out of those bytes, so they do
    not need pinning to a SHA and survive a Steam update. Returns {} unless the
    whole chain matches and the call target is a real function entry.
    """
    lo_off = hi_off = None
    for _, addr, off, size in secs:
        if addr and addr <= anchor_lo < addr + size:
            lo_off = off + (anchor_lo - addr)
            hi_off = off + (anchor_hi - addr)
            break
    if lo_off is None:
        return {}
    body = data[lo_off:hi_off]

    # add eax,imm32 ; push eax ; call rel32
    m = re.search(rb"\x05(....)\x50\xe8(....)", body, re.S)
    if not m:
        return {}
    cache_off = struct.unpack("<I", m.group(1))[0]
    rel = struct.unpack("<i", m.group(2))[0]
    call_end = anchor_lo + m.end()              # rel32 is relative to the next insn
    target = (call_end + rel) & 0xFFFFFFFF
    f = fde_for(target)
    if not f or f[0] != target:
        return {}                               # not a function entry: refuse

    # Walk back for `mov eax,[ebp+disp32]` feeding that deref, then find where
    # the same slot was written from a `lea reg,[GOT+disp32]`.
    pre = body[:m.start()]
    m2 = None
    for m2 in re.finditer(rb"\x8b\x85(....)", pre, re.S):
        pass                                    # last one before the call
    if not m2:
        return {"GetPackageInfo": target, "_cache_off": cache_off}
    slot = m2.group(1)

    glob = None
    for st in re.finditer(rb"\x89\x85" + re.escape(slot), pre, re.S):
        # The lea is not necessarily adjacent to the store; GCC interleaves
        # unrelated setup between them (sub esp / mov ebx,edi in this build).
        # Scan a short window back and take the closest lea to the store.
        window = pre[max(0, st.start() - 24):st.start()]
        last = None
        for ml in re.finditer(rb"\x8d[\x80-\xbf](....)", window, re.S):
            last = ml
        if last:
            glob = (got + struct.unpack("<i", last.group(1))[0]) & 0xFFFFFFFF
            break

    out = {"GetPackageInfo": target, "_cache_off": cache_off}
    if glob is not None:
        out["CPackageInfoCacheGlobal"] = glob
    return out


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

    # Derived, not pinned: read them out of CheckAppOwnership's own call site,
    # which VProf already located. These survive a Steam update.
    if component == "steamclient" and "CheckAppOwnership" in found:
        anchor_lo = found["CheckAppOwnership"]
        f = fde_for(anchor_lo)
        if f:
            derived = derive_from_call_site(data, secs, got, fde_for, f[0], f[1])
            cache_off = derived.pop("_cache_off", None)
            for name, rva in derived.items():
                found[name] = rva
                print(f"  {name:<31} -> 0x{rva:X}  (derived from CheckAppOwnership)",
                      file=sys.stderr)
            if cache_off is not None:
                print(f"  {'cache subobject offset':<31} 0x{cache_off:X}  "
                      f"(must match kPackageInfoCacheOffset in Hooks_Package.cpp)",
                      file=sys.stderr)

    if component == "steamclient" and "CUtlMemoryGrow" not in found:
        grow, klass = derive_cutlmemory_grow(data, secs, fde_for)
        if grow:
            found["CUtlMemoryGrow"] = grow
            print(f"  {'CUtlMemoryGrow':<31} -> 0x{grow:X}  (derived: 4-byte "
                  f"CUtlMemory<T>::Grow, {klass} identical instantiations)",
                  file=sys.stderr)
        else:
            print(f"  {'CUtlMemoryGrow':<31} NOT DERIVED (largest candidate class "
                  f"was {klass}; too few to trust)", file=sys.stderr)

    # Merge in structurally-verified addresses, but only for the exact binary
    # they were confirmed against.
    pinned = VERIFIED.get(sha)
    if pinned:
        for name, rva in pinned.items():
            if name in found:
                continue   # already derived
            if fde_for(rva) and fde_for(rva)[0] != rva:
                print(f"  {name:<31} REJECTED: 0x{rva:X} is not an FDE start", file=sys.stderr)
                continue
            found[name] = rva
            print(f"  {name:<31} -> 0x{rva:X}  (verified, pinned to this sha256)", file=sys.stderr)
    else:
        # Only complain about what is actually still missing: the derivation
        # pass above may already have recovered some of the pinned names.
        missing = sorted(n for n in REQUIRED[component] if n not in found)
        # CUtlMemoryGrow is load-bearing: without it the injected package cannot
        # grow its app list, so ownership silently does nothing. The other two
        # only drive the live license refresh.
        critical = [n for n in missing if n in ("CUtlMemoryGrow",)]

        if missing:
            print(f"  Steam build sha {sha[:16]}... is not one this generator has\n"
                  f"  been calibrated against. Still unresolved: {', '.join(missing)}",
                  file=sys.stderr)
        if critical:
            print(f"  {', '.join(critical)} cannot be derived automatically and is\n"
                  f"  required for ownership injection. Installing without it would\n"
                  f"  give a hook set that looks healthy and unlocks nothing.\n"
                  f"  Refusing. Pass --partial to install anyway.", file=sys.stderr)
            if not args.partial:
                return 1
        elif missing:
            print(f"  None of those are required for ownership injection; "
                  f"continuing.", file=sys.stderr)

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
    sys.exit(main())
