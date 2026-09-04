# Changelog

Every released version, newest first. **Config → Updates** reads this file from
the tracked branch and shows it in the client, so the format is load-bearing:
one `## <version> — <date>` heading per release, prose beneath it.

## 1.0.3 — 2026-09-03

Downloading a fix works. It never has: `[fixes] token` asked for a value that
cannot be pasted, because what that header wants is a Supabase access token
minted per session and expired within the hour. The route that suits a client
with no browser of its own turned out to be the one lua.tools' own app uses —
Discord — and it is now a button on the Fixes and Sources pages. It opens
Discord's normal authorise page in the real browser and lands back on the
module's loopback server, PKCE throughout, so the code that comes back is worth
nothing without a verifier that never leaves this process. Only the rotating
refresh token is kept. The download itself was wrong twice over as well,
writing a JSON link to disk as a `.zip` and never sending the slot.

Three manifest sources come with that session. Luie, TwentyTwo Cloud and
Skyflare are served only through lua.tools' proxy and were unreachable before.
They are listed either way now and say they need a sign-in, because "you have
no account" and "this app is not carried" are different answers. Ryuu and Sushi
stay direct, since routing them through the proxy would spend one of the 25
daily downloads on bytes that are already free.

Fixes can also be applied, for the one step they all open with. The archive
still lands in `<Steam>/steamflipper/fixes/` and applying is still a separate
decision, but "extract into the game folder" is mechanical — the folder is one
Steam chose and may be on another drive — so a button does it, over a `.sfbak`
of anything it replaces. Everything past extracting stays with whoever read the
instructions: across a sample of the catalog, 96% of fixes are extract-and-play,
and for the rest an archive carrying a `.cmd` or `.exe` says so rather than
letting *Extracted* read as finished. Each fix also links to the release that
made it, which for 1630 of the 1769 games in the catalog is online-fix.me.

Signing in is offered from the download that needed it, so a refusal carries
the button that fixes it and resumes itself afterwards. Config no longer
scrolls 418px past the bottom of its own dialog.

## 1.0.2 — 2026-09-03

The nav dropdown is a real Steam popup window, so it no longer loses to the
browser views on Store, Community and the profile, and nothing has to be hidden
or moved to make room. Those pages are native surfaces composited over the
client window's DOM, which no menu drawn in that DOM can win against however it
is stacked; Steam answers this for its own nav menus by making each one a
top-level window, and this does the same. It rests on Steam internals, so it
feature-detects, keeps the in-page menu as a fallback, and `[ui] popup_menu =
false` forces that fallback.

An uncalibrated Steam build also stops accusing itself. The missing-function
popup listed every unresolved name, including a dozen belonging to features
this port does not carry, and asked for a bug report at the upstream Windows
tracker. It now separates what ownership injection actually needs — naming the
fix before asking for a report — from what was never derived on Linux.

## 1.0.1 — 2026-09-03

Hubcap as a third manifest source, wired into the probe, the installer and a
Sources page that holds the key and picks the order sources are tried in. The
website's own Activate button works, caught off the URL Steam logs before it
dispatches it. Links open in the system browser.

The version a build reports now follows the `VERSION` file instead of whatever
the first CMake configure cached, which is why 1.0.1 is the first release whose
number can be trusted from inside the client.

## 1.0.0 — 2026-09-03

A standalone Linux port of OpenSteamTool, with an in-client UI.

Ownership injection and depot decryption from Lua manifests, a LUAFlipper tab
inside Steam for finding, managing and fixing them, cloud saves for apps the
account does not own, and hook patterns the module re-derives itself when Steam
updates underneath an install.
