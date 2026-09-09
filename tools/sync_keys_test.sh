#!/usr/bin/env bash
# =============================================================================
# Runnable check for tools/sync_keys.sh
#
#   ./tools/sync_keys_test.sh
#
# Same method as auto_update_test.sh, and for the same reason: this script runs
# detached, closes the client that asked for it, and reports only through a
# file. The case that matters most is the one nobody would notice going wrong,
# so it is checked explicitly below: config.vdf must never be written while
# Steam is still up, because Steam rewrites it on exit and would throw the keys
# away again.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBJECT="${SCRIPT_DIR}/sync_keys.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

fails=0
check() {
	if [[ "$1" == "yes" ]]; then printf '  ok   %s\n' "$2"
	else printf '  FAIL %s\n' "$2"; fails=$((fails + 1)); fi
}
is() { [[ "$1" == "$2" ]] && echo yes || echo no; }
# -- because the patterns here start with one: without it grep reads "--steam"
# as an option, fails, and `lacks` reports a pass it never checked.
has() { grep -qF -- "$2" "$1" && echo yes || echo no; }
lacks() { grep -qF -- "$2" "$1" && echo no || echo yes; }

# --- the stubs ---------------------------------------------------------------
mkdir -p "${TMP}/bin"
cat >"${TMP}/bin/pgrep" <<'EOF'
#!/usr/bin/env bash
f="${STUB_STATE}/alive_$2"   # pgrep -x <name>
n=$(cat "$f" 2>/dev/null || echo 0)
[[ "$n" -le 0 ]] && exit 1
echo $((n - 1)) >"$f"
echo 12345
EOF
cat >"${TMP}/bin/sleep" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"${TMP}/bin/setsid" <<'EOF'
#!/usr/bin/env bash
exec "$@"
EOF
cat >"${TMP}/bin/steam" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "${*:-start}" >>"${STUB_STATE}/steam.calls"
EOF
# Records how sync_depot_keys.py was invoked, so the --steam passthrough can be
# checked without a real config.vdf to write.
cat >"${TMP}/bin/python3" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${STUB_STATE}/python.calls"
exit "${STUB_SYNC_RC:-0}"
EOF
chmod +x "${TMP}/bin/"*
export PATH="${TMP}/bin:${PATH}"

REPO="${TMP}/repo"
mkdir -p "${REPO}/tools"
echo "9.9.9" >"${REPO}/VERSION"
touch "${REPO}/tools/sync_depot_keys.py"

run_case() {
	STATE="${TMP}/state_$1"
	export STUB_STATE="${STATE}"
	mkdir -p "${STATE}"
	echo "${2}" >"${STATE}/alive_steam"
	: >"${STATE}/steam.calls"
	: >"${STATE}/python.calls"
	STUB_SYNC_RC="${3}" "${SUBJECT}" "${REPO}" "${STATE}" "/opt/steam/steam" ${4:+"$4"}
	STATUS_STATE="$(sed -n 1p "${STATE}/update-status")"
}

echo "Steam closes and the keys are written"
run_case ok 2 0 "/home/u/.local/share/Steam"
check "$(is "${STATUS_STATE}" ok)" "reported ok"
check "$(is "$(grep -c . "${STATE}/steam.calls")" 2)" "Steam was closed and started"
check "$(is "$(sed -n 1p "${STATE}/steam.calls")" "-shutdown")" "closed first"
check "$(is "$(sed -n 2p "${STATE}/steam.calls")" "start")" "started after"
check "$(has "${STATE}/python.calls" "sync_depot_keys.py")" "the sync script ran"
check "$(has "${STATE}/python.calls" "--steam /home/u/.local/share/Steam")" \
      "the Steam directory is passed through, not left to be guessed"
check "$(has "${STATE}/update-status" "Retry the download")" \
      "says what to do next"

echo "No Steam directory given"
run_case nodir 2 0
check "$(is "${STATUS_STATE}" ok)" "reported ok"
check "$(lacks "${STATE}/python.calls" "--steam")" \
      "the flag is omitted entirely rather than passed empty"

echo "The sync fails"
run_case syncfail 2 1
check "$(is "${STATUS_STATE}" failed)" "reported failed"
check "$(is "$(sed -n 2p "${STATE}/steam.calls")" "start")" \
      "Steam is started anyway, so a failed write does not cost the session"
check "$(has "${STATE}/update-status" "keysync.log")" "points at the log"

echo "Steam will not close"
run_case stuck 999 0
check "$(is "${STATUS_STATE}" failed)" "reported failed"
check "$(is "$(grep -c . "${STATE}/steam.calls")" 1)" \
      "only the shutdown was sent; Steam was never restarted"
# The one that would actually hurt: Steam rewrites config.vdf as it exits, so a
# write underneath a running client is silently undone.
check "$(is "$(grep -c . "${STATE}/python.calls")" 0)" \
      "config.vdf was not touched while Steam was up"
check "$(has "${STATE}/update-status" "no keys were written")" \
      "says nothing was written"

echo "A repo with no sync script"
STATE="${TMP}/state_noscript"
export STUB_STATE="${STATE}"
mkdir -p "${STATE}"
echo 2 >"${STATE}/alive_steam"
: >"${STATE}/steam.calls"
BARE="${TMP}/bare"; mkdir -p "${BARE}/tools"
"${SUBJECT}" "${BARE}" "${STATE}" "/opt/steam/steam"
check "$(is "$(sed -n 1p "${STATE}/update-status")" failed)" "reported failed"
check "$(is "$(grep -c . "${STATE}/steam.calls")" 0)" \
      "Steam was never closed, because the run could not have worked"

echo "The status file"
check "$(is "$(wc -l <"${TMP}/state_ok/update-status")" 4)" \
      "four lines, the same shape the update helper writes"
check "$(has "${TMP}/state_ok/keysync.log" "repo ${REPO}")" \
      "the log names what it worked on"

if [[ "${fails}" -eq 0 ]]; then echo; echo "all passed"; else echo; echo "${fails} FAILED"; fi
exit $(( fails > 0 ? 1 : 0 ))
