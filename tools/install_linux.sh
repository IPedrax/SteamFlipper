#!/usr/bin/env bash
# =============================================================================
# SteamFlipper — one-shot install for the Linux Steam client
#
#   ./tools/install_linux.sh                    build + install SteamFlipper
#   ./tools/install_linux.sh --no-build         install from an existing build/
#   ./tools/install_linux.sh --with-millennium  also wire up Millennium (opt-in)
#   ./tools/install_linux.sh --no-millennium    and stop doing so
#   ./tools/install_linux.sh --uninstall        restore Steam to stock
#
# Everything lands under $HOME — nothing system-wide, no root, no PATH changes.
#
# Four things have to be in place for the client to actually work, and each of
# them has bitten someone already, so the installer does all four:
#
#   1. the 32-bit module            the Steam CLIENT is 32-bit; a 64-bit-only
#                                   build loads nowhere useful
#   2. the libXtst bootstrap        how the module gets in without LD_PRELOAD
#   3. pattern files                per-Steam-build hook addresses; regenerated
#                                   here because a Steam update invalidates them
#   4. depot keys in config.vdf     otherwise content downloads but will not
#                                   decrypt ("Missing decryption key")
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LIBDIR="${HOME}/.local/lib/steamflipper"
BOOTSTRAP_SRC="${REPO_ROOT}/src/Bootstrap/Linux/sf_bootstrap.c"
BUILD_TYPE="${SF_BUILD_TYPE:-Release}"

# Our bootstrap embeds one of these; the stock libXtst embeds neither. Used to
# avoid ever backing up our own proxy on top of the real library — doing that
# once left a system with no working libXtst at all.
#
# BST_RUNTIME_PATH is the pre-rename marker. It must stay in this pattern: an
# installer that only recognised the new name would treat an already-installed
# older proxy as the stock library and "back it up" over the genuine one.
MARKER_RE="SF_RUNTIME_PATH|BST_RUNTIME_PATH"

# Remembers whether Millennium was wired up, so re-running after a Steam update
# restores the same setup instead of silently dropping it. A Steam update
# overwrites ubuntu12_64/libXtst.so.6, so "did I set this up before?" cannot be
# answered by looking at the files.
STATE_FILE="${LIBDIR}/install-state"

say()  { printf '[*] %s\n' "$*"; }
warn() { printf '[!] %s\n' "$*" >&2; }
die()  { printf '[x] %s\n' "$*" >&2; exit 1; }

# --- locate Steam ------------------------------------------------------------
find_steam() {
    for d in "${SF_STEAM_DIR:-}" "${HOME}/.local/share/Steam" \
             "${HOME}/.steam/steam" "${HOME}/.steam/root"; do
        [ -n "${d}" ] && [ -d "${d}/ubuntu12_32" ] && { echo "${d}"; return; }
    done
    return 1
}

# --- uninstall ---------------------------------------------------------------
uninstall() {
    say "Removing SteamFlipper"
    local steam xtst orig
    if steam="$(find_steam)"; then
        xtst="${steam}/ubuntu12_32/libXtst.so.6"
        orig="${xtst}.sf-orig"
        if [ -f "${orig}" ] && ! grep -qaE "${MARKER_RE}" "${orig}" 2>/dev/null; then
            mv -f "${orig}" "${xtst}"
            say "    restored stock libXtst.so.6"
        elif [ -f "${orig}" ]; then
            # A backup that is itself a proxy (possible if an older installer
            # mis-detected one). Drop it and fall through to the runtime copy.
            rm -f "${orig}"
            warn "    discarded a backup that was itself a proxy"
        elif [ -f "${xtst}" ] && grep -qaE "${MARKER_RE}" "${xtst}" 2>/dev/null; then
            # Our proxy with no backup beside it. Steam's own runtime ships a
            # pristine copy; recover from there rather than leaving a stub that
            # forwards to nothing.
            local pinned
            # Must be the i386 copy: steam-runtime ships both arches, and a
            # 64-bit libXtst dropped into ubuntu12_32/ is worse than none --
            # the 32-bit client then fails to resolve it at all.
            pinned=""
            for cand in $(find "${steam}/ubuntu12_32/steam-runtime" \
                               -path '*i386-linux-gnu*' -name 'libXtst.so.6*' \
                               -type f 2>/dev/null); do
                if file -b "${cand}" 2>/dev/null | grep -q 'ELF 32-bit'; then
                    pinned="${cand}"; break
                fi
            done
            if [ -n "${pinned}" ]; then
                install -Dm755 "${pinned}" "${xtst}"
                say "    restored 32-bit libXtst.so.6 from the steam-runtime copy"
            else
                # Deleting is safer than leaving our proxy in place: Steam
                # restores the file on next update or "verify integrity".
                rm -f "${xtst}"
                warn "    removed our libXtst proxy but found no 32-bit stock copy;"
                warn "    Steam will restore it on next update, or verify files."
            fi
        fi
        # Millennium's 64-bit side, if we installed it.
        local xtst64="${steam}/ubuntu12_64/libXtst.so.6"
        if [ -f "${xtst64}.sf-orig" ]; then
            mv -f "${xtst64}.sf-orig" "${xtst64}"
            rm -f "${steam}/ubuntu12_64/libmillennium_hhx64.so"
            say "    restored stock 64-bit libXtst.so.6"
        fi
        rm -rf "${steam}/ubuntu12_32/steamflipper/pattern" 2>/dev/null || true
        say "    removed generated pattern files"
    fi
    rm -rf "${LIBDIR}"          # includes install-state
    say "    removed ${LIBDIR}"
    # Legacy layout from the pre-bootstrap installer.
    rm -f "${HOME}/.local/bin/bst-steam"   # legacy name
    if [ -f "${HOME}/.local/share/applications/steam.desktop" ] && \
       grep -q "bst-steam" "${HOME}/.local/share/applications/steam.desktop" 2>/dev/null; then
        rm -f "${HOME}/.local/share/applications/steam.desktop"
        say "    removed legacy desktop override"
    fi
    say "Done. Steam launches unmodified."
    say "Note: depot keys already written into config.vdf are left in place —"
    say "they are inert without the module and removing them would lose data."
}

# Spelled out rather than `[ a ] || [ b ] && { ... }`: under `set -e` that list
# evaluates to non-zero when neither matches, which is a trap waiting to happen.
# Default to whatever last time did, so --no-build after a Steam update is
# enough. An explicit --with-millennium / --no-millennium always wins.
WITH_MILLENNIUM=0
MILLENNIUM_EXPLICIT=0
if [ -f "${STATE_FILE}" ] && grep -q '^millennium=1$' "${STATE_FILE}" 2>/dev/null; then
    WITH_MILLENNIUM=1
fi
NO_BUILD=0
for arg in "$@"; do
    case "${arg}" in
        --uninstall|-u)     uninstall; exit 0 ;;
        --no-build)         NO_BUILD=1 ;;
        --with-millennium)  WITH_MILLENNIUM=1; MILLENNIUM_EXPLICIT=1 ;;
        --no-millennium)    WITH_MILLENNIUM=0; MILLENNIUM_EXPLICIT=1 ;;
        -h|--help)
            sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "unknown option '${arg}' (try --help)" ;;
    esac
done

# --- prerequisites -----------------------------------------------------------
say "Checking prerequisites"
missing=()
for t in cmake ninja python3 gcc; do
    command -v "${t}" >/dev/null 2>&1 || missing+=("${t}")
done
[ ${#missing[@]} -gt 0 ] && die "missing tools: ${missing[*]}"

# The client is 32-bit, so a multilib toolchain is mandatory. Detect it by
# actually compiling rather than guessing at distro package names.
deps_hint() {
    warn "  Arch    : pacman -S --needed gcc-multilib lib32-glibc lib32-openssl lib32-curl"
    warn "  Debian  : dpkg --add-architecture i386 && apt update &&"
    warn "            apt install gcc-multilib g++-multilib libc6-dev-i386 \\"
    warn "                        libssl-dev:i386 libcurl4-openssl-dev:i386"
    warn "  Fedora  : dnf install glibc-devel.i686 libstdc++-devel.i686 \\"
    warn "                        openssl-devel.i686 libcurl-devel.i686"
}

if ! echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
    warn "gcc cannot build 32-bit binaries — install multilib support:"
    deps_hint
    die  "32-bit toolchain required"
fi

# OpenSSL is the one external system library the 32-bit link needs (everything
# else is built from source). CMake's CMAKE_FIND_LIBRARY_CUSTOM_LIB_SUFFIX is a
# *preference*, not a restriction, so without lib32-openssl it silently resolves
# the host's 64-bit libcrypto, configures fine, and dies much later at link with
# "file in wrong format" — naming no package. Catch it here, by name.
if ! echo 'int main(void){return 0;}' | gcc -m32 -x c - -lcrypto -o /dev/null 2>/dev/null; then
    warn "32-bit OpenSSL (libcrypto) is missing — the build would fail at link"
    warn "with an opaque 'file in wrong format'. Install the 32-bit libraries:"
    deps_hint
    die  "32-bit OpenSSL required"
fi
say "    32-bit toolchain + libraries OK"

STEAM_DIR="$(find_steam)" || die "no Steam install found (looked for ubuntu12_32/); set SF_STEAM_DIR"
say "    Steam at ${STEAM_DIR}"

if pgrep -x steam >/dev/null 2>&1; then
    die "Steam is running — close it first: steam -shutdown"
fi

# --- build -------------------------------------------------------------------
if [ "${NO_BUILD}" -eq 0 ]; then
    say "Building the 32-bit module (${BUILD_TYPE}) — this takes a while"
    "${REPO_ROOT}/tools/build_linux.sh" "${BUILD_TYPE}" 32
fi

MODULE="${REPO_ROOT}/build/32/SteamFlipper.so"
[ -f "${MODULE}" ] || die "no module at ${MODULE}; run without --no-build"

# --- install the module ------------------------------------------------------
# Copied out of build/ deliberately: a clean rebuild wipes build/, and the file
# Steam loads on every launch should not vanish mid-iteration.
install -Dm755 "${MODULE}" "${LIBDIR}/32/SteamFlipper.so"
say "Installed module -> ${LIBDIR}/32/SteamFlipper.so"

# --- build and install the bootstrap ----------------------------------------
XTST="${STEAM_DIR}/ubuntu12_32/libXtst.so.6"
TMP_BOOT="$(mktemp -d)"; trap 'rm -rf "${TMP_BOOT}"' EXIT
gcc -m32 -shared -fPIC -O2 -o "${TMP_BOOT}/libXtst.so.6" "${BOOTSTRAP_SRC}" -ldl \
    || die "bootstrap failed to build"

# Back up the stock library exactly once, and never back up our own proxy.
if [ -f "${XTST}" ] && ! grep -qaE "${MARKER_RE}" "${XTST}" 2>/dev/null; then
    if [ ! -f "${XTST}.sf-orig" ]; then
        cp -a "${XTST}" "${XTST}.sf-orig"
        say "Backed up stock libXtst.so.6 -> libXtst.so.6.sf-orig"
    fi
fi
install -Dm755 "${TMP_BOOT}/libXtst.so.6" "${XTST}"
say "Installed bootstrap -> ${XTST}"

# The 64-bit client library must stay stock: the module is 32-bit, and a proxy
# there stops steamwebhelper (the UI process) from starting.
if [ -f "${STEAM_DIR}/ubuntu12_64/libXtst.so.6" ] && \
   grep -qaE "${MARKER_RE}" "${STEAM_DIR}/ubuntu12_64/libXtst.so.6" 2>/dev/null; then
    warn "ubuntu12_64/libXtst.so.6 is a SteamFlipper proxy — that breaks steamwebhelper."
    warn "Restore it from ubuntu12_64/libXtst.so.6.sf-orig if present."
fi

# --- pattern files -----------------------------------------------------------
# Hook addresses are per-Steam-build and keyed by SHA-256, so this must be re-run
# after every client update. The generator fails safe: if it does not recognise
# the binary it emits nothing rather than a stale address.
# --- optional: Millennium 64-bit side ----------------------------------------
# Millennium injects into steamwebhelper through ubuntu12_64/libXtst.so.6. Its
# own installer puts a SYMLINK there pointing into /usr/lib/millennium, which is
# a host path the sniper container running steamwebhelper cannot resolve -- the
# loader silently falls through to the system libXtst and the 64-bit half never
# loads. That is why Millennium's config page and frontend extensions go missing.
#
# Copying real files into ubuntu12_64 fixes it: everything under the Steam dir is
# visible inside the container, and the hhx64 bootstrap locates its core with
# dladdr on itself, which resolves beside the copy.
MILL_DIR=/usr/lib/millennium
if [ "${WITH_MILLENNIUM}" -eq 1 ] && [ -f "${MILL_DIR}/libmillennium_bootstrap_hhx64.so" ]; then
    if [ "${MILLENNIUM_EXPLICIT}" -eq 0 ]; then
        say "Millennium: re-applying (remembered from the last install)"
    else
        say "Millennium detected, wiring up its 64-bit side"
    fi
    XTST64="${STEAM_DIR}/ubuntu12_64/libXtst.so.6"
    if [ -f "${XTST64}" ] && ! grep -qa "hhx64" "${XTST64}" 2>/dev/null; then
        [ -f "${XTST64}.sf-orig" ] || cp -a "${XTST64}" "${XTST64}.sf-orig"
        say "    backed up stock 64-bit libXtst.so.6"
    fi
    # Real files, never symlinks (see above).
    install -Dm755 "${MILL_DIR}/libmillennium_hhx64.so" \
                   "${STEAM_DIR}/ubuntu12_64/libmillennium_hhx64.so"
    install -Dm755 "${MILL_DIR}/libmillennium_bootstrap_hhx64.so" "${XTST64}"
    # Millennium drives the frontend over the CEF debugging protocol.
    touch "${STEAM_DIR}/.cef-enable-remote-debugging"
    say "    installed hhx64 bootstrap + core into ubuntu12_64"
    MILLENNIUM_READY=1
fi

if [ "${WITH_MILLENNIUM}" -eq 0 ] && [ -f "${MILL_DIR}/libmillennium_bootstrap_hhx64.so" ]; then
    say "Millennium detected but not touched (default). Add --with-millennium to set it up."
fi

mkdir -p "${LIBDIR}"
printf 'millennium=%s\n' "${WITH_MILLENNIUM}" > "${STATE_FILE}"

# --- migrate the pre-rename runtime directory --------------------------------
# The module reads patterns and writes logs under <Steam>/steamflipper/. Before
# the rename that was <Steam>/opensteamtool/, so an existing install would find
# an empty directory and silently install no hooks. Move it once rather than
# making the user notice.
OLD_DATA="${STEAM_DIR}/ubuntu12_32/opensteamtool"
NEW_DATA="${STEAM_DIR}/ubuntu12_32/steamflipper"
if [ -d "${OLD_DATA}" ] && [ ! -d "${NEW_DATA}" ]; then
    mv "${OLD_DATA}" "${NEW_DATA}"
    say "Migrated ${OLD_DATA##*/} -> ${NEW_DATA##*/} (pre-rename data)"
fi

# Same for the config file. Missing this silently discards the user's settings —
# a suppressed-popup preference, for one, comes straight back on next launch.
OLD_CFG="${STEAM_DIR}/opensteamtool.toml"
NEW_CFG="${STEAM_DIR}/steamflipper.toml"
if [ -f "${OLD_CFG}" ] && [ ! -f "${NEW_CFG}" ]; then
    mv "${OLD_CFG}" "${NEW_CFG}"
    say "Migrated opensteamtool.toml -> steamflipper.toml (pre-rename config)"
fi

# steamui.so is intentionally not generated: hooking it segfaults the client on
# startup, reproduced across several address sets including independently
# verified ones. Those hooks only drive library cosmetics and app-removal, not
# unlocking, so leaving them off costs nothing. The generator refuses them by
# default too (--allow-steamui to override while debugging).
say "Generating hook patterns for this Steam build"
PATTERNS_OK=1
if [ -f "${STEAM_DIR}/ubuntu12_32/steamclient.so" ]; then
    # Note the pipefail-safe form: the generator's exit status matters, but it
    # is on the left of a pipe. Without PIPESTATUS we would read sed's status
    # and call a refusal a success — exactly the banner bug being fixed.
    "${REPO_ROOT}/tools/gen_linux_patterns.py" \
        "${STEAM_DIR}/ubuntu12_32/steamclient.so" --install > "${TMP_BOOT}/pat.log" 2>&1 || PATTERNS_OK=0
    sed 's/^/    /' "${TMP_BOOT}/pat.log"
else
    PATTERNS_OK=0
    warn "    no steamclient.so found"
fi

# --- depot decryption keys ---------------------------------------------------
# Created here so a first-time install has somewhere to drop manifests, and so
# the key sync below reports "none yet" instead of failing on a missing path.
mkdir -p "${STEAM_DIR}/config/stplug-in"

say "Syncing depot decryption keys into config.vdf"
"${REPO_ROOT}/tools/sync_depot_keys.py" --steam "${STEAM_DIR}" 2>&1 | sed 's/^/    /' || \
    warn "    depot key sync failed; downloaded content may not decrypt"

if [ "${PATTERNS_OK}" -eq 0 ]; then
    cat >&2 <<EOF

==========================================
SteamFlipper is installed, but NOT FUNCTIONAL on this Steam build.

  Hook addresses could not be generated for your steamclient.so. Most of
  them are pinned to specific Steam builds and cannot be re-derived
  automatically, so ownership/DLC unlocking will silently do nothing.

  Everything else (bootstrap, depot keys) is in place. Re-run this
  installer after the addresses have been updated for your build.
==========================================
EOF
    exit 1
fi

cat <<EOF

==========================================
SteamFlipper is installed.

  Just launch Steam normally — no wrapper, no LD_PRELOAD.

  disable once   : SF_DISABLE=1 steam
  uninstall      : ./tools/install_linux.sh --uninstall
  after a Steam update, re-run this to refresh hook addresses:
                   ./tools/install_linux.sh --no-build

  Put Lua manifests in:
    ${STEAM_DIR}/config/stplug-in/
  then re-run tools/sync_depot_keys.py (Steam closed) so their
  depot keys reach config.vdf.

  Logs: ${STEAM_DIR}/ubuntu12_32/steamflipper/main.log
==========================================
EOF

if [ "${MILLENNIUM_READY:-0}" = "1" ]; then
    cat <<EOF
Millennium is set up alongside SteamFlipper. Launch Steam with:

    LD_PRELOAD=${MILL_DIR}/libmillennium_x86.so steam

Millennium must load via LD_PRELOAD, not from SteamFlipper's proxy: it hooks
main() and needs the dynamic loader to map it before Steam initialises. Loading
it any later stops steamwebhelper from starting.

==========================================
EOF
fi
