#!/usr/bin/env bash
# =============================================================================
# Runnable check for tools/auto_update.sh
#
#   ./tools/auto_update_test.sh
#
# Runs the real script against stubs. `pgrep`, `sleep`, `setsid` and the Steam
# binary are shadowed on PATH, and the repo it is pointed at is a temporary one
# with a stub installer, so the three outcomes can be forced without closing
# anything or waiting two minutes for a timeout.
#
# Worth having because this script runs exactly when nothing is watching: it is
# launched detached, the UI that asked for it is about to be killed by it, and
# its only report is a file. A mistake here is one nobody sees until Steam does
# not come back.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBJECT="${SCRIPT_DIR}/auto_update.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

fails=0
check() {
	if [[ "$1" == "yes" ]]; then printf '  ok   %s\n' "$2"
	else printf '  FAIL %s\n' "$2"; fails=$((fails + 1)); fi
}
is() { [[ "$1" == "$2" ]] && echo yes || echo no; }
has() { grep -qF "$2" "$1" && echo yes || echo no; }
lacks() { grep -qF "$2" "$1" && echo no || echo yes; }

# --- the stubs ---------------------------------------------------------------
#
# pgrep answers from a countdown file, so "Steam is still up" can be made to
# last exactly as long as a case needs. sleep returns immediately, which is the
# only reason the timeout case is testable at all.
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
# Stands in for ubuntu12_32/steam, which is what the module actually passes.
# Every call it receives is a call that should have gone to the launcher.
cat >"${TMP}/bin/steam_inner" <<'EOF'
#!/usr/bin/env bash
printf 'inner %s\n' "${*:-start}" >>"${STUB_STATE}/steam.calls"
EOF
chmod +x "${TMP}/bin/"*
export PATH="${TMP}/bin:${PATH}"

# A repo with a stub installer whose exit code the case chooses.
REPO="${TMP}/repo"
mkdir -p "${REPO}/tools"
echo "9.9.9" >"${REPO}/VERSION"
cat >"${REPO}/tools/install_linux.sh" <<'EOF'
#!/usr/bin/env bash
echo "stub installer ran"
exit "${STUB_INSTALL_RC:-0}"
EOF
chmod +x "${REPO}/tools/install_linux.sh"

run_case() {
	STATE="${TMP}/state_$1"
	export STUB_STATE="${STATE}"
	mkdir -p "${STATE}"
	echo "${2}" >"${STATE}/alive_steam"          # polls before Steam "closes"
	echo 0      >"${STATE}/alive_steamwebhelper"
	: >"${STATE}/steam.calls"
	STUB_INSTALL_RC="${3}" "${SUBJECT}" "${REPO}" "${STATE}" "${TMP}/bin/steam_inner"
	STATUS_STATE="$(sed -n 1p "${STATE}/update-status")"
	STATUS_MSG="$(sed -n 3p "${STATE}/update-status")"
}

echo "Steam closes and the build works"
run_case ok 2 0
check "$(is "${STATUS_STATE}" ok)" "reported ok"
check "$(has "${STATE}/update.log" "stub installer ran")" "the installer ran"
check "$(is "$(grep -c . "${STATE}/steam.calls")" 2)" "Steam was closed and started"
check "$(is "$(sed -n 1p "${STATE}/steam.calls")" "-shutdown")" "closed first"
check "$(is "$(sed -n 2p "${STATE}/steam.calls")" "start")" "started after"
check "$(has "${STATE}/update-status" "9.9.9")" "the new version is named"
check "$(is "$(sed -n 2p "${STATE}/update-status")" \
        "$(date '+%Y-%m-%d %H:%M')")" "stamped with the time it finished"
check "$(lacks "${STATE}/steam.calls" "inner")" \
      "the launcher on PATH is used, not the inner binary the module reports"

echo "The build fails"
run_case build 1 1
check "$(is "${STATUS_STATE}" failed)" "reported failed"
check "$(is "$(sed -n 2p "${STATE}/steam.calls")" "start")" \
      "Steam is started anyway, so a bad build does not cost the session"
check "$(has "${STATE}/update-status" "previous module is still installed")" \
      "says the old build is still in place"

echo "Steam will not close"
run_case stuck 999 0
check "$(is "${STATUS_STATE}" failed)" "reported failed"
check "$(is "$(grep -c . "${STATE}/steam.calls")" 1)" \
      "only the shutdown was sent; Steam was never restarted"
# The one that would actually hurt: overwriting the library Steam still has
# mapped.
check "$(lacks "${STATE}/update.log" "stub installer ran")" \
      "the installer did not run while Steam was up"
check "$(has "${STATE}/update-status" "nothing was installed")" \
      "says nothing was installed"

echo "The status file"
# It has to say something from the first moment, or a helper that is still
# compiling is indistinguishable from one that died.
check "$(has "${TMP}/state_ok/update.log" "repo ${REPO}")" \
      "the log names what it worked on"
check "$(is "$(wc -l <"${TMP}/state_ok/update-status")" 4)" \
      "four lines: state, time, sentence, version"
check "$(is "$(sed -n 4p "${TMP}/state_ok/update-status")" "9.9.9")" \
      "the fourth line is the version, so a repeat failure can be skipped"

if [[ "${fails}" -eq 0 ]]; then echo; echo "all passed"; else echo; echo "${fails} FAILED"; fi
exit $(( fails > 0 ? 1 : 0 ))
