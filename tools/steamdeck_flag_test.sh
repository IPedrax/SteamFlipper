#!/usr/bin/env bash
# =============================================================================
# Runnable check for the steamdeck_flag block in tools/install_linux.sh
#
#   ./tools/steamdeck_flag_test.sh
#
# The function is extracted from the installer and run against a throwaway HOME,
# so the edits happen to fixture files rather than to whoever runs this.
#
# Worth having because this one edits files the installer did not create. The
# case that would actually hurt is a sed that mangles an unrelated launcher, so
# the flag has to come out and everything around it has to survive untouched.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLER="${SCRIPT_DIR}/install_linux.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

fails=0
check() {
	if [[ "$1" == "yes" ]]; then printf '  ok   %s\n' "$2"
	else printf '  FAIL %s\n' "$2"; fails=$((fails + 1)); fi
}
is() { [[ "$1" == "$2" ]] && echo yes || echo no; }

# The function, its two helpers and the flag it reads, lifted out of the
# installer so this tests the shipping text rather than a copy of it.
extract() {
	{
		echo 'say()  { printf "[*] %s\n" "$*"; }'
		echo 'warn() { printf "[!] %s\n" "$*" >&2; }'
		echo "STRIP_STEAMDECK=${1}"
		sed -n '/^steamdeck_flag() {$/,/^}$/p' "${INSTALLER}"
		echo 'steamdeck_flag'
	} >"${TMP}/subject.sh"
}

# pgrep is shadowed to report no Steam, so the running-process branch stays out
# of the way of the file-editing cases.
mkdir -p "${TMP}/bin"
printf '#!/usr/bin/env bash\nexit 1\n' >"${TMP}/bin/pgrep"
chmod +x "${TMP}/bin/pgrep"
export PATH="${TMP}/bin:${PATH}"

fixture() {
	local home="${TMP}/home_$1"
	rm -rf "${home}"
	mkdir -p "${home}/.config/autostart" "${home}/.local/share/applications"
	printf 'Exec=/usr/bin/steam -steamdeck -silent %%U\nName=Steam\n' \
		>"${home}/.config/autostart/steam.desktop"
	# Must survive untouched: no flag, and a name that contains the word.
	printf 'Exec=/usr/bin/steam %%U\nName=Steam (Desktop)\n' \
		>"${home}/.local/share/applications/steam-desktop.desktop"
	echo "${home}"
}

echo "Reporting only, which is the default"
HOME_DIR="$(fixture report)"
extract 0
OUT="$(HOME="${HOME_DIR}" bash "${TMP}/subject.sh" 2>&1)"
check "$(grep -q 'sets it:' <<<"${OUT}" && echo yes || echo no)" \
      "names the file that sets the flag"
check "$(grep -q -- '--no-steamdeck' <<<"${OUT}" && echo yes || echo no)" \
      "says how to remove it"
check "$(grep -q -- '-steamdeck' "${HOME_DIR}/.config/autostart/steam.desktop" \
        && echo yes || echo no)" \
      "changes nothing without being asked"

echo "Removing it, when asked"
HOME_DIR="$(fixture strip)"
extract 1
OUT="$(HOME="${HOME_DIR}" bash "${TMP}/subject.sh" 2>&1)"
AUTO="${HOME_DIR}/.config/autostart/steam.desktop"
check "$(grep -q -- '-steamdeck' "${AUTO}" && echo no || echo yes)" \
      "the flag is gone"
check "$(is "$(sed -n 1p "${AUTO}")" 'Exec=/usr/bin/steam -silent %U')" \
      "everything else on the line survives, spacing included"
check "$(is "$(sed -n 2p "${AUTO}")" 'Name=Steam')" "other lines are untouched"
check "$(test -f "${AUTO}.sf-orig" && echo yes || echo no)" \
      "the original is kept alongside"
check "$(grep -q -- '-steamdeck' "${AUTO}.sf-orig" && echo yes || echo no)" \
      "and the copy still has the flag, so the edit is reversible"

echo "A launcher that never had the flag"
UNTOUCHED="${HOME_DIR}/.local/share/applications/steam-desktop.desktop"
check "$(is "$(sed -n 1p "${UNTOUCHED}")" 'Exec=/usr/bin/steam %U')" \
      "is left exactly as it was"
check "$(test -f "${UNTOUCHED}.sf-orig" && echo no || echo yes)" \
      "and is not backed up, because it was not edited"

echo "Nothing to find"
HOME_DIR="${TMP}/home_empty"
mkdir -p "${HOME_DIR}/.config/autostart"
extract 1
OUT="$(HOME="${HOME_DIR}" bash "${TMP}/subject.sh" 2>&1)"
check "$(is "${OUT}" "")" "says nothing at all when no flag is set anywhere"

if [[ "${fails}" -eq 0 ]]; then echo; echo "all passed"; else echo; echo "${fails} FAILED"; fi
exit $(( fails > 0 ? 1 : 0 ))
