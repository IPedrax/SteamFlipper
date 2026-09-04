#!/usr/bin/env bash
# =============================================================================
# SteamFlipper — close Steam, rebuild, reinstall, start Steam again
#
#   ./tools/auto_update.sh <repo> <state-dir> <steam-binary>
#
# Launched detached by the module from Config → Updates, after the pull has
# already succeeded. It has to outlive the process that started it: the module
# being replaced is loaded into Steam, so Steam must be gone before the
# installer runs, which means this script is still working long after the code
# that asked for it has exited.
#
# Nothing here is interactive and nothing needs root. Everything it does lands
# in the log next to the state file, because by the time it matters there is no
# UI left to report into.
# =============================================================================
set -uo pipefail

REPO="${1:?repo path}"
STATE="${2:?state directory}"
STEAM="${3:?steam binary}"

LOG="${STATE}/update.log"
STATUS="${STATE}/update-status"

# The module reports the running executable, which for the Steam client is
# ubuntu12_32/steam -- the inner binary, started by a launcher that sets up the
# runtime around it. Starting that directly skips all of it, so the launcher on
# PATH is preferred and the inner binary is only the fallback for a Steam
# installed without one.
STEAM="$(command -v steam || echo "${STEAM}")"

mkdir -p "${STATE}"
exec >"${LOG}" 2>&1

say() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*"; }

# The status file is the whole report. The page that asked for this is inside
# the Steam that is about to close, so the next session reads the outcome from
# here: first line the state, second the time, third a sentence.
finish() {
	printf '%s\n%s\n%s\n' "$1" "$(date '+%Y-%m-%d %H:%M')" "$2" >"${STATUS}"
	say "$1: $2"
	exit "${3:-0}"
}

printf 'running\n%s\nUpdating.\n' "$(date '+%Y-%m-%d %H:%M')" >"${STATUS}"
say "repo ${REPO}"
say "version $(cat "${REPO}/VERSION" 2>/dev/null || echo unknown)"

# --- 1. Steam has to go ------------------------------------------------------
#
# -shutdown is Steam's own graceful exit, so games save and the client writes
# its config out. Then wait: the client takes a few seconds to actually leave,
# and the installer overwrites the very library still mapped into it.
say "asking Steam to close"
"${STEAM}" -shutdown >/dev/null 2>&1 || true

for _ in $(seq 1 120); do
	pgrep -x steam >/dev/null 2>&1 || break
	sleep 1
done
if pgrep -x steam >/dev/null 2>&1; then
	finish failed "Steam did not close within two minutes, so nothing was installed." 1
fi

# steamwebhelper outlives the client by a moment and holds the CEF port.
for _ in $(seq 1 20); do
	pgrep -x steamwebhelper >/dev/null 2>&1 || break
	sleep 1
done
say "Steam is closed"

# --- 2. build and install ----------------------------------------------------
#
# The installer does the whole job: the 32-bit module, the bootstrap, the
# pattern files for whichever Steam build is now on disk, and the depot keys.
say "running the installer"
if ! "${REPO}/tools/install_linux.sh"; then
	# Steam comes back regardless. A failed update should cost the user their
	# session for as long as the build took, not until they notice.
	say "installer failed, starting Steam anyway"
	setsid "${STEAM}" >/dev/null 2>&1 &
	finish failed "The build failed. The previous module is still installed; see update.log." 1
fi

# --- 3. back up --------------------------------------------------------------
say "starting Steam"
setsid "${STEAM}" >/dev/null 2>&1 &

finish ok "Updated to $(cat "${REPO}/VERSION" 2>/dev/null || echo 'a new build') and restarted Steam."
