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
| **A tab inside Steam** | Find and add manifests, manage what you have, browse fixes and change settings without leaving the client. No Millennium, no separate app |
| **Cloud saves** | Manifest-added apps get no Steam Cloud, because the account does not own them. SteamFlipper answers those requests itself, out of a folder on this machine |
| **Self-calibrating** | Steam updates change every hook address. The module notices and re-derives them itself instead of going quiet |
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

## <img src="assets/icons/gauge.svg" width="20" align="absmiddle" alt="" /> The LUAFlipper tab

After installing, Steam has one more tab next to your account name. Hover it for the menu; it borrows the client's own markup and class names, so a custom theme restyles it along with everything else.

| Page | What it is |
|---|---|
| **Unlocker** | Steam's real store, opened as this tab. Prices render as a 100% discount and **Add to Cart** installs a manifest instead of adding to the cart. Leave the tab and the store is exactly as Valve shipped it |
| **Manage** | Your manifests as a library grid. Hover a game for its key counts and to update or remove it. A removal is undoable until the next Steam start, and deleted then |
| **Fixes** | The games you have a manifest for that a published fix exists for, laid out like a library game page. The fix opens with its own instructions, downloads the archive, and offers to extract it over the game |
| **Config** | Steam's settings dialog, for this module: updates, cloud saves, configuration and status. **Updates** shows what a new version changes and applies it in one click |

The store integration only applies while the Unlocker tab is the open one. The module hands it out as a lease the tab has to keep renewing, so anything that ends the tab — a navigation, a crashed script, closing it — puts the real store back within seconds. The Store tab is never touched.

Turn the whole thing off with `[ui] enabled = false` in `steamflipper.toml`.

> The store needs Steam's CEF debugger, which the installer enables by creating `<Steam>/.cef-enable-remote-debugging`. It listens on loopback only.

### Cloud saves

Apps added by a manifest are not on your account, so Valve answers their cloud uploads with *Access Denied* and those games end up with no cloud at all. SteamFlipper answers the `Cloud.*` requests itself and keeps the files under `<Steam>/steamflipper/cloudsaves`. Only apps with a manifest in `config/stplug-in` are answered; games you actually own keep using Valve's cloud and are never touched.

Switch it on under **Config → Cloud saves**. It takes effect on the next Steam start.

> Those saves live only on this machine. Nothing copies them anywhere else, so point a backup or sync client at that folder if they matter.

### Downloading fixes

The fix catalog is readable without an account. Downloading one is not — that endpoint wants a lua.tools session, and the cap of 25 downloads a day is per account. **Sign in with Discord** on the Fixes or Sources page opens Discord's normal authorise page in your browser and lands back on the module's own loopback server. It is PKCE throughout, so the code that comes back is worth nothing without a verifier that never leaves the process, and only the rotating refresh token is stored.

There is nothing to paste. A `[fixes] token` in the config never worked and has been dropped: what that header wants is an access token minted per session, so any value written to a file was stale within the hour.

The same session unlocks three manifest sources — Luie, TwentyTwo Cloud and Skyflare — which are served only through lua.tools' proxy. Ryuu and Sushi stay direct, because routing them through it would spend one of the 25 to fetch bytes that are already free.

The archive lands in `<Steam>/steamflipper/fixes/`. Applying it is a separate button, and only ever does the one step every fix opens with: **Extract to game folder**, into the folder Steam chose — found by walking `libraryfolders.vdf`, so a game on another drive works. Anything it replaces is copied to `.sfbak` first, once, so the DLLs the game shipped survive a second fix.

Everything past extracting stays with whoever read the instructions, because that part differs per release: across a sample of the catalog, 96% of fixes are extract-and-play, and the rest ask for a config edit or an installer run afterwards. If the archive carries a `.cmd`, `.exe` or `.vbs`, the result says so rather than letting *Extracted* read as finished.

---

## <img src="assets/icons/file.svg" width="20" align="absmiddle" alt="" /> Adding manifests by hand

The Unlocker tab does this for you, but nothing here depends on it. A manifest is a plain Lua file, and adding one by hand takes three steps.

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

Adding a manifest **while Steam is running** works too. A file watcher registers the ownership right away. Whether the library *view* redraws without a restart depends on two addresses that are pinned per Steam build, so on an uncalibrated one the app is owned but the list still needs a restart to show it. You still need step 2, with Steam closed, before its content will download.

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

### Updating SteamFlipper

**Config → Updates** compares this build against the `VERSION` file on the branch it was built from and shows that release's changelog. If there is a newer one, **Update and restart Steam** does the whole thing: it pulls, closes Steam, builds, installs and starts Steam again.

It has to be that way round rather than done in place. The installer replaces the module that is running the button, so Steam must be gone before it can, which means a detached helper carries it out and there is nothing left to report into. The outcome is written to `<Steam>/steamflipper/update-status` and shown at the top of the same page when Steam comes back, with the full build log next to it in `update.log`.

Nothing is forced: a build that fails leaves the previous module installed and starts Steam again anyway. Point `[update] repo` at your checkout for any of it to work.

---

## <img src="assets/icons/settings.svg" width="20" align="absmiddle" alt="" /> After a Steam update

Nothing. Hook addresses are byte offsets keyed by the SHA-256 of `steamclient.so`, and a client update changes every one of them — but the module notices that the file it just hashed has no pattern set and re-derives them itself on that launch. The generator runs from a copy the installer leaves beside the module, outside the steam-runtime, whose `readelf` and pinned libraries would otherwise break it.

This used to be the sharpest edge in the whole project: the pattern file stopped matching **silently**, Steam ran normally, and nothing unlocked. It was reported from a Steam Deck, where client updates land often enough that most installs live in the gap between an update and a published pattern set.

If you ever want to force it by hand:

```bash
steam -shutdown
./tools/install_linux.sh --no-build
```

Everything ownership injection needs is derived from the binary, so an uncalibrated build still unlocks. A handful of extras — live library refresh among them — are pinned per build and stay off until someone publishes a set for it. The generator says which, and continues rather than refusing.

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
