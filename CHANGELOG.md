# Changelog

Every released version, newest first.

**Config → Updates** reads this file from the tracked branch and shows one
entry: the version you are on, or the one an update would bring. So the format
is load-bearing — a `## <version> — <date>` heading per release, `-` bullets
beneath it, each short enough to read in a settings panel.

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
