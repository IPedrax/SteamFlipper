# Installing SteamFlipper

Two parts. The first happens in a terminal and is one command. The second
happens inside Steam and is one click.

There is an interactive version of this page — pick your distribution, copy
each command, tick the steps off — but everything it says is below.

> Needs Linux and the Steam client. Nothing is written outside `$HOME`, nothing
> needs root, and `./tools/install_linux.sh --uninstall` puts Steam back to
> stock. Budget about ten minutes, nearly all of it compiling.

---

## Part one — install

### 1. Install the build tools

The Steam client is a 32-bit binary, so a multilib toolchain is not optional.
The installer checks by actually compiling and linking, and names the missing
package rather than failing later with `file in wrong format`.

```bash
# Arch · Manjaro · CachyOS
sudo pacman -S --needed base-devel cmake ninja git \
        gcc-multilib lib32-glibc lib32-openssl lib32-curl

# Debian · Ubuntu · SteamOS
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install build-essential cmake ninja-build gcc-multilib \
        g++-multilib libc6-dev-i386 libssl-dev:i386 libcurl4-openssl-dev:i386

# Fedora · Nobara
sudo dnf install @development-tools cmake ninja-build \
        glibc-devel.i686 libstdc++-devel.i686 \
        openssl-devel.i686 libcurl-devel.i686
```

### 2. Close Steam

A step of its own because skipping it is the usual way this goes wrong. The
installer replaces a library the running client holds open, and with Steam up
it refuses rather than half-installing.

```bash
steam -shutdown
```

### 3. Clone and run the installer

```bash
git clone https://github.com/IPedrax/SteamFlipper.git
cd SteamFlipper
./tools/install_linux.sh
```

One command does the whole job: it builds the 32-bit module, installs the
loader that gets it into Steam, generates the hook patterns for whichever Steam
build is on your disk, and registers the depot keys.

### 4. Start Steam and check the top bar

Launch Steam however you normally do. There is one more tab, after your account
name: **LUAFLIPPER**. It borrows the client's own markup, so a custom Steam skin
restyles it too.

No tab means the module did not load. Check
`grep -i steamflipper ~/.local/share/Steam/logs/bootstrap_log.txt` before
anything else — and check that step 2 actually happened, since installing over
a running Steam is the common cause.

---

## Part two — add a manifest

### 5. Open LUAFLIPPER, then Unlocker

Hover the tab for its menu and pick **Unlocker**. What opens is not a copy of
the store — it is Steam's real store, opened as this tab. Search and browse
exactly as you always would.

### 6. Find a game and look at the buy box

The purchase box has changed: the price is struck through and shown as a 100%
discount, and the green button now reads **Add to LUAFlipper**.

This applies only on this tab. Leave it, or open the ordinary Store tab, and
the store is exactly as Valve shipped it — the integration is a lease the tab
has to keep renewing, so anything that ends the tab puts the real store back
within seconds.

### 7. Click Add to LUAFlipper

The button reports as it goes: `Adding 3s`, then `In library` once the manifest
is written, or `No source` if none of the configured sources carry that app.
Nothing reaches your real cart — the click never gets to Steam's own handler.

### 8. Restart Steam, then check Manage

Steam reads manifests once, at startup, so the game appears in your library on
the next launch and not before. To confirm what you have, open **LUAFLIPPER →
Manage** and hover any tile: each shows how many decryption keys and app ids its
manifest carries, and can be updated or removed. A removal stays undoable until
the next Steam start.

---

## Adding one by hand

Nothing here depends on the Unlocker. A manifest is a plain Lua file in
`~/.local/share/Steam/config/stplug-in/`, named after the app id —
`1245620.lua` for Elden Ring — holding one `addappid` line per depot with its
decryption key:

```lua
-- 381210.lua  |  Dead by Daylight
addappid(381210, 1, "f17be424bd1dc965706d5527f803a0c4ae2c7aae87449be6f5bab17b7ac3a20c")
addappid(381211, 1, "66477a849ed619510d127ff3f05d4d37b38a04fd7074654337053dfe817798ca")
```

Drop one in and restart Steam; Manage will list it with the rest.

---

## If something is wrong

**The installer stopped on a missing package.** It names the one it needs.
Install that and run it again — it is safe to re-run as many times as you like.

**The tab is there but pages never finish loading.** A stale listener on port
1987, fixed in 1.0.5. Close Steam fully and start it again;
`ss -ltnp | grep 1987` should then show exactly one process.

**A game was added but is not in the library.** Steam only reads manifests at
startup, so restart it. If it is still missing, open **Manage** — if the tile is
not there either the add did not land, and **Sources** will say which hosts were
tried.

**Steam updated and nothing unlocks.** It should fix itself: hook addresses are
keyed to the hash of `steamclient.so`, and the module re-derives them on the
first launch after a client update. See *After a Steam update* in the README.

---

## Where to go next

| Page | What it is for |
|---|---|
| **Unlocker** | Steam's real store, with Add to Cart rerouted |
| **Workshop** | Steam's real workshop, with Subscribe rerouted to download |
| **Manage** | Every manifest you have, as a library grid |
| **Fixes** | Published fixes for a game, and extracting one into its folder |
| **Config** | Updates and the changelog, cloud saves, sources, status |

The porting notes that used to live in this file — why the module is
dual-architecture, how the `libXtst` bootstrap works, what the installer does
and why — are in the README and in the repository history.
