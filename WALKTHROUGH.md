# Walkthrough: SteamFlipper Linux Port

Status: **builds and links cleanly for both architectures. Runtime injection into a
live Steam session has not been verified yet**, see [Status](#status) at the end
before assuming a feature works.

---

## Why the port is dual-architecture

The Linux Steam client is still a **32-bit** binary. On a current install the main
process is `ubuntu12_32/steam` (ELF 32-bit i386), and it is that process which maps
the `steamclient.so` and `steamui.so` the hooks target:

```
$ file ~/.local/share/Steam/ubuntu12_32/steam
ELF 32-bit LSB pie executable, Intel i386

$ grep -oE '/[^ ]*(steamclient|steamui)\.so' /proc/$(pgrep -f ubuntu12_32/steam)/maps | sort -u
/home/…/.local/share/Steam/ubuntu12_32/steamclient.so
/home/…/.local/share/Steam/ubuntu12_32/steamui.so
```

Only the CEF UI helper (`steamwebhelper`) is 64-bit, and it maps
`linux64/steamclient.so`. So:

- a **32-bit** module is required to hook the Steam client core, and
- the **64-bit** module remains useful for game-side and Proton hooking.

Both are built and both are preloaded. The dynamic loader ignores the entry whose
ELF class does not match the process it is loading into, logging
`cannot be preloaded (wrong ELF class): ignored`, which is expected and harmless.
This is exactly the pattern Steam itself uses: it ships `gameoverlayrenderer.so` in
both `ubuntu12_32/` and `ubuntu12_64/` for the same reason.

Note there is **no `ubuntu12_64/steamclient.so`** on a modern install, that
directory holds the CEF/webhelper stack. The arch-specific directory name has
drifted across releases (`ubuntu12_*`, `linux*`, `steamrt*`), so module paths are
probed at runtime rather than hardcoded.

---

## Key Changes

### 1. Build system
- **`src/CMakeLists.txt`**: removed the Windows-only early return; added the Linux
  shared library target; `CMAKE_POSITION_INDEPENDENT_CODE ON` so the static deps can
  be linked into a shared object; Windows PE loader hijacks guarded to `WIN32`.
- **`src/cmake/Funchook.cmake`**: funchook as the inline hooking engine (works on
  both i386 and x86_64).
- **`src/cmake/Linux32.cmake`**: i386 toolchain file. Sets `-m32` for C, C++, ASM
  and the linkers, and steers `find_package` at `/usr/lib32`. The ASM flag matters
  on its own: funchook declares `LANGUAGES C ASM` and hand-writes
  `prehook-i686-gas.S`, and ASM flags do not inherit from the C flags.
- **`src/cmake/FetchCache.cmake`**: the cached-dependency list is now
  platform-aware. Detours is Windows-only and funchook/distorm are Linux-only, so
  the old unconditional list left the "all cached" check permanently false on Linux
  which pinned every dependency's *build* tree to the shared `.deps/` cache. With
  two architectures that meant one shared `libfunchook.a` and the arches clobbering
  each other.

### 2. SFPlatform Linux backend (`src/SFPlatform/Linux/`)

| Component | Source | Implementation |
| :--- | :--- | :--- |
| Detour engine | `Detour.cpp` | funchook inline hooks |
| Dynamic loader | `DynamicLibrary.cpp` | `dlopen`/`dlsym`, `/proc/self/exe` |
| Memory | `Memory.cpp` | `dl_iterate_phdr` for image bounds, `mprotect` for patching |
| Directory watcher | `DirectoryWatch.cpp` | `inotify` + `poll` |
| HTTP | `Http.cpp` | libcurl |
| Hashing | `Hash.cpp` | OpenSSL `EVP` SHA-256 |
| Process inspection | `Process.cpp` | `/proc/[pid]/{exe,cmdline,environ,maps,stat}` |
| Remote process | `RemoteProcess.cpp` | ELF `EI_CLASS` (handles both classes) |
| Credential store | `SteamCredentialStore.cpp` | file-based cache + VDF parsing |
| Threads | `Thread.cpp` | detached `pthread_create` |
| Traps | `Trap.cpp` | `sigaction` + `ucontext_t`; **x86_64 and i386** register access |
| Pattern search | `ByteSearch.cpp` | Boyer-Moore-Horspool over `pread` |
| Proton PE parser | `PE.cpp` | PE header parser for Wine/Proton binaries |

`Trap.cpp` originally guarded every accessor with `#if defined(__x86_64__)` and
returned 0 otherwise, so it would have been silently inert on the 32-bit client. It
now reads `REG_EIP`/`REG_ESP` and resolves cdecl stack arguments on i386. Register
values are narrowed through `uintptr_t` before widening, because `greg_t` is signed
and a raw cast sign-extends a normal `0xBFFF_xxxx` stack pointer.

### 3. Entry point (`src/dllmain.cpp`)
- `__attribute__((constructor))` / `((destructor))` for `LD_PRELOAD` injection.
- **Host gating now applies on Linux.** `LD_PRELOAD` is inherited by every process
  Steam spawns, `steamwebhelper`, the runtime launcher, reaper, Proton and the
  games themselves, so the constructor fires far more often than `DllMain` ever
  did. Without the gate each child would load steamclient, install detours and start
  watchers off its own (wrong) working directory.
- Module paths are probed across the arch-appropriate candidate directories rather
  than assuming `ubuntu12_64/`.

### 4. Cross-platform fixes
- `src/Steam/Structs.h`, the `KeyValues` size assertion is pointer-size dependent
  (`0x20` on 64-bit, `0x18` on 32-bit); both figures verified against what the
  compiler emits, with the 64-bit layout matching `steamclient64`.
- `src/SFPlatform/Encoding.cpp`, wide/UTF-8 conversion via the C library.
- `src/Utils/Update/AppUpdater.cpp`, `std::filesystem` rename/remove.
- `src/Utils/Tokeer/*`, Windows registry and shell APIs guarded.

---


## Install

One command, from a clean checkout:

```bash
./tools/install_linux.sh
```

Everything lands under `$HOME`, no root, no system files, no `PATH` changes.
Afterwards just launch Steam normally; there is no wrapper to remember.

It needs a **multilib toolchain**, because the Steam client is 32-bit. The
installer verifies this by actually compiling a 32-bit binary rather than
guessing at package names, and tells you the right command for your distro if it
fails:

```bash
# Arch
sudo pacman -S --needed base-devel cmake ninja git \
                        gcc-multilib lib32-glibc lib32-openssl lib32-curl
# Debian / Ubuntu / SteamOS
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install build-essential cmake ninja-build gcc-multilib g++-multilib \
                 libc6-dev-i386 libssl-dev:i386 libcurl4-openssl-dev:i386
# Fedora
sudo dnf install @development-tools cmake ninja-build \
                 glibc-devel.i686 libstdc++-devel.i686 \
                 openssl-devel.i686 libcurl-devel.i686
```

The **32-bit OpenSSL** package is not optional. CMake's
`CMAKE_FIND_LIBRARY_CUSTOM_LIB_SUFFIX` is a search *preference*, not a
restriction, so without it the configure step silently resolves the host's
64-bit `libcrypto`, succeeds, and the build dies much later at link with
`file in wrong format`, naming no package. The installer link-tests for it up
front so the failure names what to install.

### What the installer does

Four things have to line up for the client to work, and each has its own failure
mode, so all four are automated:

| Step | Why it matters if missing |
| :--- | :--- |
| 32-bit module → `~/.local/lib/steamflipper/32/` | The **client** is 32-bit; a 64-bit-only build hooks nothing |
| `libXtst.so.6` bootstrap → `ubuntu12_32/` | How the module loads without `LD_PRELOAD` |
| Pattern files → `ubuntu12_32/steamflipper/pattern/` | Hook addresses; without them no hook installs |
| Depot keys → `config/config.vdf` | Content downloads but fails with *Missing decryption key* |

Steam must be closed during install (`steam -shutdown`); the installer refuses
otherwise, because Steam rewrites `config.vdf` on exit and would discard the keys.

### Why `libXtst.so.6` and not `LD_PRELOAD`

`LD_PRELOAD` is inherited by every process Steam spawns, steamwebhelper, the
runtime, reaper, Proton, and the games. The module does nothing in those
children, but it is still *mapped* into each one, which is a needless anti-cheat
risk inside a game. Steam links `libXtst.so.6` directly, so a proxy in its place
is resolved only by processes that actually link it: the client, and nothing else.

The 64-bit `ubuntu12_64/libXtst.so.6` must stay the **real** library. The module
is 32-bit, and a proxy there stops steamwebhelper (the UI process) from starting.
The installer warns if it finds one.

## After a Steam update

Hook addresses are byte offsets into `steamclient.so` / `steamui.so`, keyed by
SHA-256. A client update changes every one of them, and the pattern files stop
matching, silently, because they are looked up by hash. Symptom: Steam runs
normally but nothing unlocks.

```bash
steam -shutdown
./tools/install_linux.sh --no-build     # regenerates patterns, skips the rebuild
```

The generator fails safe: given a binary it does not recognise it emits nothing
rather than a stale address. That matters because a wrong address does not fail
politely, funchook patches unrelated memory and Steam segfaults on launch.

## Adding Lua manifests

Drop them in `<Steam>/config/stplug-in/`. They are picked up live, no restart.

Their **depot decryption keys** are a separate step, because Steam reads those
from `config.vdf`:

```bash
steam -shutdown
./tools/sync_depot_keys.py          # --dry-run to preview
```

Ownership works without this; decryption does not. A manifest with no
`addappid(<depot>, 1, "<key>")` line carries no key at all, and nothing can
decrypt that depot, check the Lua before assuming the tool is at fault.

## Running Millennium alongside

Both can run at once. They must use their own injection paths, SteamFlipper via
the `libXtst` proxy, Millennium via its own `LD_PRELOAD`:

```bash
LD_PRELOAD=/usr/lib/millennium/libmillennium_x86.so steam
```

Verified together in one session: 13 `steamwebhelper` processes, Millennium
loading 5 plugins, SteamFlipper injecting 1007 apps.

What does **not** work is co-loading Millennium from SteamFlipper's proxy (the
retired `MILLENNIUM_ENABLE=1`). That is the long-standing "Millennium breaks the
UI" symptom, and the cause is load order rather than configuration: Millennium
hooks `main()` and rewrites `LD_PRELOAD` for the steamwebhelper child, both of
which need it mapped by the dynamic loader before Steam initialises. A `dlopen`
from the proxy's constructor is too late, and priming `LD_PRELOAD` first does not
compensate.

Isolation, for the record:

| configuration | `steamwebhelper` |
| :--- | :--- |
| Millennium off | 12 |
| Millennium co-loaded, stock 64-bit libXtst | 0 |
| Millennium co-loaded + 64-bit bootstrap + hhx64 symlink | 0 |
| Millennium via `LD_PRELOAD` | 13 |

Leave `ubuntu12_64/libXtst.so.6` as the stock library. And note that Millennium's
64-bit bootstrap resolves its core with `dladdr` on itself, which returns the
*symlink* path, so it needs `ubuntu12_64/libmillennium_hhx64.so` to exist. Some
packages omit that symlink; it is a genuine bug, but adding it does not by itself
revive steamwebhelper.

## Troubleshooting

Build `Debug` for logs, `STEAMFLIPPER_LOGGING_ENABLED` is only defined for
`$<CONFIG:Debug>`, so a Release build compiles logging out entirely:

```bash
SF_BUILD_TYPE=Debug ./tools/install_linux.sh
```

Logs are written next to the host executable:
`~/.local/share/Steam/ubuntu12_32/steamflipper/main.log`.

| Symptom | Check |
| :--- | :--- |
| Nothing unlocks | `grep "Hook: attached" main.log`, zero means patterns are stale; re-run the installer |
| Steam segfaults on launch | A wrong hook address. `./tools/install_linux.sh --uninstall`, then re-run to regenerate |
| *Missing decryption key* | `./tools/sync_depot_keys.py --dry-run`; if it reports 0 to add, the Lua has no key for that depot |
| Games owned but absent from the library | `grep InitFakeLicense main.log`, the injected package is how they get listed |

Escape hatches, in increasing order of severity:

```bash
SF_DISABLE=1 steam                    # one launch, untouched
./tools/install_linux.sh --uninstall   # restore Steam to stock
```

Uninstall restores the stock `libXtst.so.6` from the backup it made, or from the
steam-runtime's own copy if that backup is gone. Depot keys already written into
`config.vdf` are deliberately left alone, they are inert without the module, and
removing them would throw away data the user may have collected.

---

## Status

Working and verified on a live client (Arch, 32-bit Steam client):

- Module loads via the `libXtst` bootstrap; no `LD_PRELOAD`, launches from the app menu.
- Hooks attach at kernel-verified addresses (`/proc/<pid>/maps` base + RVA).
- Lua manifests parse, including files added while Steam is running.
- Ownership injection works: apps appear in the injected package with
  `ExistInPackageNums=1`, and DLC lists populate.
- Depot decryption via `config.vdf`.

Known limits:

- **Hook addresses are per-build, but most are now derived.** Of the nine
  located by RVA, three come from VProf scopes and three more are derived from
  those: `GetPackageInfo` and the `CPackageInfoCache` owner global are read out
  of the call `CheckAppOwnership` already makes, and `CUtlMemoryGrow` is found
  as a 4-byte `CUtlMemory<T>::Grow` instantiation. Only `MarkLicenseAsChanged`
  and `ProcessPendingLicenseUpdates` remain pinned to a SHA-256, and neither is
  required for ownership injection, so a Steam update no longer stops the
  unlock working. The generator still refuses rather than emitting a partial
  set if something load-bearing goes missing. `tools/gen_linux_patterns.py`
  automates the recoverable ones and refuses to guess the rest.
- **`steamui.so` hooks are disabled.** Installing them segfaults the client on
  startup, reproduced across several address sets including independently
  verified ones. They drive library cosmetics and app-removal, not unlocking, so
  nothing is lost. `tools/gen_linux_patterns.py` refuses to emit them without
  `--allow-steamui`.
- `FillInAppOverview` is doubly excluded, beyond the above, the body GCC emits
  for it is unreachable (zero call sites), so a detour there could never fire.
  It only set a cosmetic purchase timestamp.
- `ConfigStoreGetBinary` is unresolved, so decryption goes through `config.vdf`
  instead of the hook. This is the more robust route anyway: it needs no
  signature and survives client updates.
- IPC interception is off; the upstream IPC spec is portable across platforms
  (only `funcHash`/`fencepost`/`argc` are read), but the two functions it hooks
  are not located yet.
