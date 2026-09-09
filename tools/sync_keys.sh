#!/usr/bin/env bash
# =============================================================================
# SteamFlipper - close Steam, write depot keys into config.vdf, start it again
#
#   ./tools/sync_keys.sh <repo> <state-dir> <steam-binary> [steam-dir]
#
# Launched detached by the module from the Unlocker page. Same shape as
# auto_update.sh and for the same reason: the work cannot happen while Steam is
# running, and the code asking for it lives inside the Steam that has to close.
#
# Why this exists at all: adding a Lua manifest registers ownership live, but
# the depot decryption key has no live path on Linux. The hook that would serve
# it needs a byte pattern for a function Valve gives no VProf scope, so the key
# goes into config.vdf instead, which Steam reads natively. Steam rewrites that
# file on exit, so an edit made underneath it is discarded. Hence: down, write,
# up.
#
# Nothing here is interactive and nothing needs root.
# =============================================================================
set -uo pipefail

REPO="${1:?repo path}"
STATE="${2:?state directory}"
STEAM="${3:?steam binary}"
STEAM_DIR="${4:-}"

LOG="${STATE}/keysync.log"
STATUS="${STATE}/update-status"

# Same reasoning as auto_update.sh: the module reports ubuntu12_32/steam, the
# inner binary, and starting that directly skips the runtime the launcher sets
# up around it.
STEAM="$(command -v steam || echo "${STEAM}")"

mkdir -p "${STATE}"
exec >"${LOG}" 2>&1

say() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*"; }

# Written to the same file the update helper uses, because the page that reads
# it cannot tell which helper ran and does not need to: both mean "Steam went
# away to do something, here is how it went".
finish() {
	printf '%s\n%s\n%s\n%s\n' "$1" "$(date '+%Y-%m-%d %H:%M')" "$2" \
		"$(cat "${REPO}/VERSION" 2>/dev/null || echo unknown)" >"${STATUS}"
	say "$1: $2"
	exit "${3:-0}"
}

printf 'running\n%s\nWriting depot keys.\n' "$(date '+%Y-%m-%d %H:%M')" >"${STATUS}"
say "repo ${REPO}"

SYNC="${REPO}/tools/sync_depot_keys.py"
[ -f "${SYNC}" ] || finish failed "tools/sync_depot_keys.py is missing from the repo." 1

# Checked before Steam is touched: a missing script is worth reporting, not
# worth costing somebody their session to discover.

# --- 1. Steam has to go ------------------------------------------------------
say "asking Steam to close"
"${STEAM}" -shutdown >/dev/null 2>&1 || true

for _ in $(seq 1 120); do
	pgrep -x steam >/dev/null 2>&1 || break
	sleep 1
done
if pgrep -x steam >/dev/null 2>&1; then
	finish failed "Steam did not close within two minutes, so no keys were written." 1
fi
say "Steam is closed"

# --- 2. write the keys -------------------------------------------------------
#
# The script backs config.vdf up itself and refuses to write when its own brace
# accounting does not add up, so a bad parse leaves the file alone.
#
# Spelled out both ways rather than built as an array, because "${args[@]}" on
# an empty array is an unbound-variable error under `set -u` before bash 4.4,
# and this runs on whatever the user's distribution ships.
say "writing depot keys"
if [ -n "${STEAM_DIR}" ]; then
	python3 "${SYNC}" --steam "${STEAM_DIR}"
else
	python3 "${SYNC}"
fi
if [ $? -ne 0 ]; then
	# Steam comes back either way. A key that did not land costs a download,
	# not the session.
	say "sync failed, starting Steam anyway"
	setsid "${STEAM}" >/dev/null 2>&1 &
	finish failed "The depot keys could not be written; see keysync.log." 1
fi

# --- 3. back up --------------------------------------------------------------
say "starting Steam"
setsid "${STEAM}" >/dev/null 2>&1 &

finish ok "Depot keys are in config.vdf. Retry the download that was stuck."
