# Changelog

Every released version, newest first.

**Config → Updates** reads this file from the tracked branch and shows one
entry: the version you are on, or the one an update would bring. So the format
is load-bearing — a `## <version> — <date>` heading per release, `-` bullets
beneath it, each short enough to read in a settings panel.

## 1.1.3 — 2026-09-03

- Fixes the workshop dialog rendering white on the hub, browse and collection pages
- Those pages are Steam's newer UI and carry none of the dialog classes, so it was an unstyled dialog
- It now detects that and takes its colours from the page itself, copying the listing's own green button, so it still follows whatever theme is in use

## 1.1.2 — 2026-09-03

- The green quick-add button on workshop listings is hooked too, not just Subscribe on an item's page
- One click on any row in the hub, browse or a collection opens the same dialog
- Matched by what makes the button green rather than by class name, since Steam's newer UI rebuilds those names on every update

## 1.1.1 — 2026-09-03

- The Workshop dialog is built to Steam's own dialog structure, so it looks like one of the client's rather than a box that resembles one
- Steam's client buttons, a close X, and the gradient top bar, all from classes a theme already targets
- Download only is the primary action, since it is the one that changes nothing outside this machine

## 1.1.0 — 2026-09-03

- New **Workshop** tab: Steam's real workshop, opened as a LUAFlipper tab, the way Unlocker opens the store
- Subscribe is rebound to ask first — subscribe and download, or download only
- The dialog is built from Steam's own classes, so a skin or plugin that themes Steam themes it too
- Downloads are done by the signed-in client itself, so depot-backed items work, not just the legacy ones a direct link can reach
- No SteamCMD and no third-party mirror
- Hooks apply only while the Workshop tab is open; the Community tab is never touched
- Fixes a libcurl crash that could take the whole client down on any HTTP call

## 1.0.9 — 2026-09-03

- The struck-through price no longer overlaps the -100% badge on a store page
- Changelog entries here render as text, instead of showing the markdown around bold and code

## 1.0.8 — 2026-09-03

- **Update on startup**, opt-in, under Config → Updates: Steam updates itself about a minute after it opens
- Off by default, because it closes the client you just opened
- Skipped while a game is running
- A version that failed to build here is never retried, so a branch that does not compile cannot close Steam on every start
- Also settable as `[update] auto_install = true`

## 1.0.7 — 2026-09-03

- Steam exits cleanly instead of aborting with a core dump every single time it closes
- A watcher thread was still joinable when its static destructor ran, and destroying a joinable thread calls `std::terminate`
- The join is registered at start time now, so it happens before that destructor rather than after
- Long-standing, and invisible: it fired after Steam had finished its own shutdown, so nothing was lost — but each one uploaded a crash report to Valve

## 1.0.6 — 2026-09-03

- Fixes a crash that could take the whole Steam client down from the Fixes or Sources page
- The lua.tools token refresh built a string from two different temporaries, then read off the end of the heap looking for one's end in the other
- Present since 1.0.3, and intermittent: it only faulted when the allocator did not hand back the same address twice

## 1.0.5 — 2026-09-03

- Fixes the 1.0.4 automatic update leaving the client with every page loading forever
- The update helper inherited the module's listening socket and passed it on to the Steam it restarted
- So the port stayed bound by a process that was gone, and the new module could not answer on it
- Detached helpers now close the descriptors they inherit, so nothing they start can hold a socket open on the process that launched them
- If 1.0.4 already did this to you, close Steam and start it again; nothing was damaged

## 1.0.4 — 2026-09-03

- Updating is one click: **Config → Updates → Update and restart Steam** pulls, closes Steam, builds, installs and starts it again
- No command to copy and no manual restart; it needs no input and takes a few minutes
- The result is waiting on that page when Steam comes back, with the build log in `update.log`
- A failed build leaves the working module installed and starts Steam anyway
- Every release now has a changelog, on GitHub and in the client
- Config → Updates shows the entry for your build, or for the update it just found
- The Config backdrop covers a long section instead of stopping one screen down

## 1.0.3 — 2026-09-03

- Fix downloads work. Sign in with Discord in your browser; nothing to paste, and the old `[fixes] token` field is gone
- Three manifest sources unlocked by that session: Luie, TwentyTwo Cloud and Skyflare
- Fixes extract into the game folder from the modal, on any drive, over a `.sfbak` of anything replaced
- An archive carrying a `.cmd` or `.exe` says so instead of reading as finished
- Each fix links to the release that made it
- The sign-in is offered from the download that needed it, and the download resumes itself
- Config stops scrolling past the bottom of its own dialog

## 1.0.2 — 2026-09-03

- The nav dropdown is a real Steam popup window, so Store, Community and the profile no longer overlap it
- Feature-detected, with the in-page menu as a fallback and `[ui] popup_menu = false` to force it
- An uncalibrated Steam build stops reporting functions this port never derives as failures, and points at the right tracker when one really is missing

## 1.0.1 — 2026-09-03

- Hubcap as a third manifest source, in the probe, the installer and the Sources page
- The source order is yours to set, and the API key can be saved from Config
- The Hubcap website's own Activate button works, caught off the URL Steam logs
- Links open in the system browser
- The version a build reports follows the `VERSION` file instead of whatever the first CMake configure cached

## 1.0.0 — 2026-09-03

- A standalone Linux port of OpenSteamTool, with an in-client UI
- Ownership injection and depot decryption from Lua manifests
- A LUAFlipper tab inside Steam for finding, managing and fixing them
- Cloud saves for apps the account does not own
- Hook patterns the module re-derives itself when Steam updates underneath an install
