# Linux injection bootstrap

`sf_bootstrap.c` is a `libXtst.so.6` proxy: Steam links that library directly
so dropping this in its place gets us loaded inside the Steam client without
`LD_PRELOAD`. The proxy re-exports the XTest entry points Steam actually uses
and forwards them to the real library.

Derived from [Millennium](https://github.com/SteamClientHomebrew/Millennium)'s
`src/bootstrap/linux/libmillennium_bootstrap.c` (MIT, see `MILLENNIUM_LICENSE`)
with a `load_steamflipper()` added next to its own core load.

## Why not LD_PRELOAD

`LD_PRELOAD` is inherited by every process Steam spawns, steamwebhelper, the
runtime launcher, reaper, Proton and the games themselves. The module gates on
the host process so it does nothing in those children, but it still gets mapped
into every one of them, which is a needless anti-cheat risk inside a game.

This proxy is resolved only by processes that link `libXtst.so.6`, so the module
lands in the Steam client and nowhere else.

## Behaviour

SteamFlipper only. Millennium is **not** co-loaded from here.

| Variable | Effect |
| :--- | :--- |
| *(none)* | Load SteamFlipper; XTest forwarding intact |
| `SF_DISABLE=1` | Skip SteamFlipper (escape hatch if a build misbehaves) |
| `SF_RUNTIME_PATH` | Override the module path |
| `MILLENNIUM_ENABLE` | **Retired**. Logs a pointer to the working method below |

## Running Millennium alongside

Co-loading Millennium from this proxy (the old `MILLENNIUM_ENABLE=1`) reliably
stopped `steamwebhelper` from starting, which is the long-standing "Millennium
breaks the UI" report. The cause is load *order*, not configuration:

- Millennium hooks `main()`, and its `Plat_HookedCreateSimpleProcess` rewrites
  `LD_PRELOAD` for the steamwebhelper child, swapping its 32-bit core for the
  hhx64 bootstrap.
- Both need Millennium to have been mapped by the dynamic loader **before** Steam
  initialises. A `dlopen` from this constructor happens too late to reproduce
  that, and priming `LD_PRELOAD` first was tried and does not help.

The two run together fine on their native, independent injection paths, SteamFlipper through this libXtst proxy, Millennium through its own `LD_PRELOAD`:

```sh
LD_PRELOAD=/usr/lib/millennium/libmillennium_x86.so steam
```

Verified: 13 `steamwebhelper` processes, Millennium loading 5 plugins, and
SteamFlipper injecting 1007 apps, all in one session.

Two related traps, if you go digging:

- Millennium's 64-bit bootstrap resolves its core with `dladdr` on *itself*, and
  `dladdr` returns the **symlink** path rather than its target. Symlinked in as
  `ubuntu12_64/libXtst.so.6`, it therefore looks for
  `ubuntu12_64/libmillennium_hhx64.so`, a third symlink upstream's installer
  creates and some packages do not. Missing it is a real bug, but adding it does
  **not** by itself fix steamwebhelper.
- `ubuntu12_64/libXtst.so.6` should stay the stock library unless you are
  deliberately running Millennium's 64-bit side.

## Build and install

```sh
gcc -m32 -shared -fPIC -O2 -o libXtst.so.6 sf_bootstrap.c -ldl
install -m755 libXtst.so.6 ~/.local/share/Steam/ubuntu12_32/libXtst.so.6
```

The 64-bit `ubuntu12_64/libXtst.so.6` must stay the **real** library, the
module is 32-bit and only belongs in the 32-bit client.

Reinstalling Millennium overwrites `ubuntu12_32/libXtst.so.6` with its own
bootstrap, so re-run the install step afterwards.
