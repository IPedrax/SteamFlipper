# SteamFlipper, integration reference

Everything an external tool (the LuaTools desktop app, its Millennium plugin, or
anything else) needs to know to drive SteamFlipper correctly on **Linux**.

SteamFlipper is a C++ module injected into the 32-bit Steam client. It reads Lua
manifests, injects app ownership into Steam's in-memory license data, and hooks
the depot-dependency path. It has **no IPC/RPC surface**. The only contract
between an external app and SteamFlipper is **files on disk** plus **when Steam
is restarted**. Get the files right and it works; there is nothing to call.

---

## 1. Locating Steam

Do **not** assume `~/.steam/steam` exists. Probe in this order and take the first
directory that contains `ubuntu12_32/`:

```
$STEAM_ROOT                 (if you define one)
~/.local/share/Steam        ← most common on Arch/Debian/Fedora
~/.steam/steam              ← a symlink on many systems, absent on others
~/.steam/root
```

Flatpak Steam lives under `~/.var/app/com.valvesoftware.Steam/.local/share/Steam`
and is not probed by SteamFlipper's own installer; if you support it, resolve it
yourself.

`<Steam>` below means that resolved directory.

## 2. Paths SteamFlipper uses

| Purpose | Path | Who writes it |
| :--- | :--- | :--- |
| Lua manifests | `<Steam>/config/stplug-in/*.lua` | **your app** |
| Depot manifests | `<Steam>/depotcache/*.manifest` | **your app** |
| Depot decryption keys | `<Steam>/config/config.vdf` → `depots` block | **your app** (see §4) |
| Module config | `<Steam>/steamflipper.toml` | user / your app |
| Category logs | `<Steam>/steamflipper/*.log` | SteamFlipper |
| Main log | `<Steam>/ubuntu12_32/steamflipper/main.log` | SteamFlipper |
| Hook addresses | `<Steam>/ubuntu12_32/steamflipper/pattern/` | installer |
| The module | `~/.local/lib/steamflipper/32/SteamFlipper.so` | installer |
| Injection point | `<Steam>/ubuntu12_32/libXtst.so.6` | installer |

Paths are derived from the **32-bit client executable's** directory, so they sit
under the resolved `<Steam>`, never under `~/.steam` unless that is where Steam
actually is.

## 3. Lua manifests

Dropped into `<Steam>/config/stplug-in/`. Format:

```lua
addappid(381210, 1, "f17be424…64 hex chars…")   -- app/depot WITH a decryption key
addappid(489980)                                 -- DLC, no key needed
addtoken(534130, "10361710686141983297")
-- setManifestid(381211, "6212006511518453098", 65284350935)
```

- The trailing `1` in `addappid(id, 1, "<key>")` means "a decryption key
  follows". The key is 64 hex characters.
- `addappid(id)` with no key registers ownership only.
- A **commented-out** `setManifestid` means "use the latest manifest", which is
  what Luie generates. Only uncomment to pin a specific manifest, and only if the
  matching `.manifest` file is in `depotcache/`.

**Live reload works.** A `.lua` added, changed, or removed while Steam is running
is picked up by an inotify watcher, no restart needed for *ownership*. (This was
broken until recently: the watcher joined paths with a hardcoded `\`, so files
added at runtime silently never loaded.)

## 4. Depot decryption keys, the part that is easy to miss

**Writing the `.lua` is not enough for content to download.** Ownership will work
and the game will appear, but the download fails with *"Missing decryption key"*.

Steam reads depot keys from `<Steam>/config/config.vdf`:

```
"depots"
{
    "2050651"
    {
        "DecryptionKey"    "c3bc4be6ae2c98c50f2ba78469906e43be35b408daf8d49f420aaf6cad275b2b"
    }
}
```

Your app must copy every `addappid(<depot>, 1, "<key>")` from its Lua files into
that block. Rules:

- **Steam must be closed.** It rewrites `config.vdf` on exit and will discard
  edits made underneath a running client.
- **Round-trip bytes, not text.** Read/write with `surrogateescape` (or raw
  bytes). Decoding with `errors="replace"` and writing back silently rewrites any
  locale-invalid byte as `U+FFFD` and corrupts unrelated fields such as account
  names and install paths.
- Write via a temp file in the same directory then rename, so an interrupted run
  cannot leave a half-written `config.vdf`.
- It is idempotent, skip depots already present.

The reference implementation is `SteamFlipper/tools/sync_depot_keys.py`; shelling out to it is
a perfectly good option:

```bash
SteamFlipper/tools/sync_depot_keys.py --steam <Steam>   # --dry-run to preview
```

A Lua with no `addappid(id, 1, "<key>")` line carries **no key at all** for that
depot, and nothing can decrypt it. Check the Lua before reporting a tool bug.

## 5. Restarting Steam

Ownership injection happens once, early in client startup, so adding Lua files
that need a *new* injected package requires a restart.

```bash
steam -shutdown        # graceful; wait for the process to actually exit
```

Two traps:

- `steam -shutdown` **hangs indefinitely if steamwebhelper never started**. The
  client cannot process the shutdown without its UI. Always bound the wait.
- Steam **ignores `SIGTERM`** in that state too. If you must force it, `SIGKILL`
  is safe *provided on-disk state is already consistent*; kill children first.

Do not make force-kill part of a normal flow.

## 6. Detecting SteamFlipper

Ordered from cheapest to most reliable:

1. **Installed**, `~/.local/lib/steamflipper/32/SteamFlipper.so` exists.
2. **Injected**, `<Steam>/ubuntu12_32/libXtst.so.6` contains the marker string
   `SF_RUNTIME_PATH` (the stock library does not).
3. **Loaded**, `SteamFlipper.so` appears in `/proc/<steam-pid>/maps`.
4. **Working**, `main.log` contains `Hook: attached CheckAppOwnership`, and
   `<Steam>/steamflipper/package.log` contains
   `InitFakeLicense(PackageId=0): adding N apps`.

**Logging only exists in Debug builds.** `STEAMFLIPPER_LOGGING_ENABLED` is
defined for `$<CONFIG:Debug>` only, so a Release install writes no logs at all.
Treat "no log file" as "Release build", not "not working", use checks 1-3.

## 7. After a Steam client update

Hook addresses are byte offsets keyed by the SHA-256 of `steamclient.so`. A Steam
update changes every one, the pattern file stops matching **silently**, and
nothing unlocks while everything else looks normal.

```bash
steam -shutdown
./SteamFlipper/tools/install_linux.sh --no-build     # regenerates, skips the rebuild
```

Most addresses cannot be re-derived automatically; the generator refuses and the
installer exits non-zero rather than claiming success. If your app wants to warn
proactively, compare the SHA-256 of `<Steam>/ubuntu12_32/steamclient.so` against
the filenames in `<Steam>/ubuntu12_32/steamflipper/pattern/steamclient/`, a
mismatch means SteamFlipper is installed but inert.

## 8. Config file

`<Steam>/steamflipper.toml` (template: `SteamFlipper/steamflipper.example.toml`). Keys worth
knowing:

```toml
[diagnostics]
popups = false          # suppress the "Missing Signatures" dialog on every launch

[manifest]
url = "opensteamtool"   # PROVIDER ID, not a product name.
                        # Valid: opensteamtool | wudrm | steamrun

[lua]
paths = ["config/stplug-in"]   # relative paths resolve against Steam's CWD
```

`opensteamtool` in `[manifest].url` is a live third-party API host and is
deliberately *not* renamed. Setting it to `steamflipper` breaks manifest lookups.

## 9. What SteamFlipper does and does not do

**Works:** ownership/DLC injection into package 0, Lua parsing with live reload
the depot-dependency hook.

**Not active on Linux:**

- **`steamui.so` hooks** are disabled. Hooking them segfaults the client, and they
  only drive library cosmetics and app-removal. Your app should not expect
  SteamFlipper to refresh the library UI; a Steam restart is the reliable way.
- **IPC interception** is off. The two functions it needs have no Linux pattern.
- **`RemoteProcess::InjectLibrary`** returns a failure status on Linux. The
  `inject_dlls` feature does nothing. It used to report success while doing
  nothing.

## 10. Running alongside Millennium

Both can run together. They must use their **own** injection paths:

```bash
LD_PRELOAD=/usr/lib/millennium/libmillennium_x86.so steam
```

Millennium's 32-bit core must be mapped by the dynamic loader **before** Steam's
`main()`, because it hooks `main()` and rewrites `LD_PRELOAD` for the
steamwebhelper child. SteamFlipper's proxy loads from a library constructor
which is strictly too late, co-loading Millennium from it stops steamwebhelper
from starting entirely.

For Millennium's **64-bit** half (its config page and frontend extensions) the
files in `<Steam>/ubuntu12_64/` must be **real files, not symlinks**:

```
ubuntu12_64/libXtst.so.6            ← copy of libmillennium_bootstrap_hhx64.so
ubuntu12_64/libmillennium_hhx64.so  ← copy of libmillennium_hhx64.so
```

Millennium's own installer puts symlinks there pointing into `/usr/lib/millennium`
a host path the sniper runtime hosting steamwebhelper cannot resolve; the loader
silently falls through to the system `libXtst` and the whole 64-bit half never
loads. `tools/install_linux.sh` detects Millennium and sets this up
(`SF_SKIP_MILLENNIUM=1` opts out). Millennium also needs
`<Steam>/.cef-enable-remote-debugging` to exist.

## 11. Quick reference

```bash
# install / refresh after a Steam update
./SteamFlipper/tools/install_linux.sh              # or --no-build
./SteamFlipper/tools/install_linux.sh --uninstall

# push Lua depot keys into config.vdf (Steam closed)
SteamFlipper/tools/sync_depot_keys.py --steam <Steam>

# escape hatch, one launch
SF_DISABLE=1 steam
```

| Variable | Effect |
| :--- | :--- |
| `SF_DISABLE=1` | Skip SteamFlipper for this launch |
| `SF_RUNTIME_PATH` | Override the module path |
| `SF_STEAM_DIR` | Tell the installer where Steam is |
| `SF_BUILD_TYPE` | `Debug` for logs (default `Release`) |
| `SF_SKIP_MILLENNIUM=1` | Do not touch Millennium during install |
