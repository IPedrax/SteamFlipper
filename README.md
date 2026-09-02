<div align="center">
  <img src="assets/icons/gauge.svg" width="56" alt="" />
  <h1>SteamFlipper</h1>
  <p><strong>A standalone Linux port of OpenSteamTool. Unlock apps and DLC on the 32-bit Steam client from Lua manifests, with no LD_PRELOAD, no wrapper script, and no launcher to remember.</strong></p>
</div>

[OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) is Windows-only. It targets Valve's MSVC x64 client, resolves its hooks from byte patterns published for those binaries, and injects itself through a DLL the Windows loader hands it. None of that exists on Linux. **SteamFlipper** is a fork that makes it work here: the hook addresses are re-derived against the GCC-built i386 `steamclient.so`, injection goes through a `libXtst.so.6` proxy that only the Steam client resolves, and the parts Linux does differently (signal-based traps, `dlopen` handles, depot decryption keys) are fixed rather than stubbed. This is a **standalone port**, not a patch set. Clone it, run one script, launch Steam normally.

Built for the **Linux Steam client** (Arch, Debian/Ubuntu, SteamOS, Fedora).

---

## <img src="assets/icons/sparkles.svg" width="20" align="absmiddle" alt="" /> What it does

| | |
|---|---|
| **Ownership injection** | Apps and DLC from your Lua manifests are added to Steam's in-memory license data, so they show up owned in the library |
| **Depot decryption** | Pushes the keys from your manifests into `config.vdf` so downloaded content actually decrypts |
| **Live manifest reload** | Drop a `.lua` in while Steam is running and it gets picked up immediately |
| **No `LD_PRELOAD`** | Loads through a `libXtst.so.6` proxy that only the Steam client resolves, so nothing is mapped into your games |
| **Millennium-compatible** | Runs alongside Millennium without breaking `steamwebhelper`. Opt-in, off by default |

---

## <img src="assets/icons/download.svg" width="20" align="absmiddle" alt="" /> Install

### <img src="assets/icons/terminal.svg" width="17" align="absmiddle" alt="" /> One command

```bash
git clone https://github.com/IPedrax/SteamFlipper.git
cd SteamFlipper
steam -shutdown
./tools/install_linux.sh
```

Then launch Steam the way you always do. Everything lands under `$HOME`, with no root, nothing system-wide, and no `PATH` changes.

### <img src="assets/icons/check.svg" width="17" align="absmiddle" alt="" /> Requirements

The Steam **client** is 32-bit, so a multilib toolchain is mandatory. The installer checks by actually compiling and linking, then names the missing package instead of failing later with `file in wrong format`.

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

### <img src="assets/icons/layers.svg" width="17" align="absmiddle" alt="" /> Options

| Command | What it does |
|---|---|
| `./tools/install_linux.sh` | **Default.** SteamFlipper only |
| `./tools/install_linux.sh --with-millennium` | Also wire up Millennium. See below |
| `./tools/install_linux.sh --no-build` | Reinstall from an existing `build/`, skipping the compile |
| `./tools/install_linux.sh --uninstall` | Restore Steam to stock |

---

## <img src="assets/icons/file.svg" width="20" align="absmiddle" alt="" /> Adding manifests by hand

You do not need the LuaTools app. A manifest is a plain Lua file, and adding one takes three steps.

**1. Write the `.lua`** into `~/.local/share/Steam/config/stplug-in/`, named after the app id:

```lua
-- 381210.lua  |  Dead by Daylight
addappid(381210, 1, "f17be424bd1dc965706d5527f803a0c4ae2c7aae87449be6f5bab17b7ac3a20c")
addappid(381211, 1, "66477a849ed619510d127ff3f05d4d37b38a04fd7074654337053dfe817798ca")

addappid(489980)   -- DLC, ownership only, no key needed
addappid(509060)
addtoken(534130, "10361710686141983297")
```

| Line | Meaning |
|---|---|
| `addappid(id, 1, "<64 hex>")` | App or depot **with** a decryption key. The `1` means a key follows |
| `addappid(id)` | Registers ownership only. Correct for DLC, which have no depot of their own |
| `addtoken(id, "<number>")` | PICS access token, for depots that need one |
| `-- setManifestid(...)` | Leave commented to use the **latest** manifest. Only uncomment to pin an old build, and only if the matching `.manifest` sits in `depotcache/` |

**2. Push the decryption keys into Steam**, with Steam closed:

```bash
steam -shutdown
./tools/sync_depot_keys.py          # --dry-run to preview
```

This is the step people miss. Ownership works from the `.lua` alone, but **content will not decrypt** without it. Steam reads depot keys from `config/config.vdf`, not from your manifest. The symptom is a download that stops with *"Missing decryption key"*.

**3. Start Steam.** The app shows up in your library.

Adding a manifest **while Steam is running** works too. A file watcher picks it up for ownership right away. You still need step 2, with Steam closed, before its content will download.

> A `.lua` with no `addappid(id, 1, "<key>")` line carries no key at all for that depot. Nothing can decrypt it. That is a gap in the manifest, not a bug in the tool.

---

## <img src="assets/icons/monitor.svg" width="20" align="absmiddle" alt="" /> Running Millennium alongside

Off by default. To set it up:

```bash
steam -shutdown
./tools/install_linux.sh --with-millennium
```

Then launch Steam with Millennium preloaded:

```bash
LD_PRELOAD=/usr/lib/millennium/libmillennium_x86.so steam
```

That `LD_PRELOAD` is not optional and cannot be folded into SteamFlipper. Millennium hooks `main()` and rewrites `LD_PRELOAD` for the `steamwebhelper` child, so it has to be mapped by the dynamic loader **before** Steam initialises. Loading it any later, including from SteamFlipper's proxy, stops `steamwebhelper` from starting at all. If you want it on every launch, put that line in a desktop entry's `Exec=` or a shell alias.

The installer also fixes the reason Millennium's **config page and frontend extensions** silently go missing. Its own installer symlinks `ubuntu12_64/libXtst.so.6` into `/usr/lib/millennium`, a host path the sniper runtime hosting `steamwebhelper` cannot resolve, so the loader falls through to the system library and the whole 64-bit half never loads. Copying real files instead fixes it.

---

## <img src="assets/icons/settings.svg" width="20" align="absmiddle" alt="" /> After a Steam update

Hook addresses are byte offsets keyed by the SHA-256 of `steamclient.so`. A client update changes every one of them, and the pattern file stops matching **silently**. Steam runs normally and nothing unlocks.

```bash
steam -shutdown
./tools/install_linux.sh --no-build
```

Most addresses cannot be re-derived automatically, so the generator refuses and the installer exits non-zero rather than telling you it worked.

---

## <img src="assets/icons/shield.svg" width="20" align="absmiddle" alt="" /> Troubleshooting

Build `Debug` for logs. `Release` compiles logging out entirely:

```bash
SF_BUILD_TYPE=Debug ./tools/install_linux.sh
```

Logs live at `~/.local/share/Steam/ubuntu12_32/steamflipper/main.log`.

| Symptom | Check |
|---|---|
| Nothing unlocks | `grep "Hook: attached" main.log`. Zero means the patterns are stale for your Steam build |
| *Missing decryption key* | `./tools/sync_depot_keys.py --dry-run`. If it reports 0 to add, the `.lua` has no key for that depot |
| Owned but not in the library | `grep InitFakeLicense main.log`. The injected package is how apps get listed |
| Steam will not start | `./tools/install_linux.sh --uninstall`, then reinstall |

Escape hatches:

```bash
SF_DISABLE=1 steam                     # one launch, untouched
./tools/install_linux.sh --uninstall   # restore Steam to stock
```

| Variable | Effect |
|---|---|
| `SF_DISABLE=1` | Skip SteamFlipper for this launch |
| `SF_STEAM_DIR` | Tell the installer where Steam is (Flatpak, custom prefix) |
| `SF_BUILD_TYPE` | `Debug` for logs, default `Release` |
| `SF_RUNTIME_PATH` | Override the module path |

---

## <img src="assets/icons/layers.svg" width="20" align="absmiddle" alt="" /> Docs

| File | Covers |
|---|---|
| [`WALKTHROUGH.md`](WALKTHROUGH.md) | How the port works: architecture, hook derivation, known limits |
| [`STEAMFLIPPER_INTEGRATION.md`](STEAMFLIPPER_INTEGRATION.md) | Everything an external app or plugin needs to drive SteamFlipper |

---

## <img src="assets/icons/file.svg" width="20" align="absmiddle" alt="" /> License

Inherits its license from [OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool). See [`LICENSE`](LICENSE). The Linux injection bootstrap derives from [Millennium](https://github.com/SteamClientHomebrew/Millennium) (MIT), see `src/Bootstrap/Linux/MILLENNIUM_LICENSE`.
