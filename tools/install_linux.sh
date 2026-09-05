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

# Steam installed as a Flatpak, which this does not support: the module gets in
# by replacing a library beside the Steam binary, and inside a Flatpak that
# library belongs to the runtime rather than to the user. Worth finding anyway,
# so the failure says what is wrong instead of "no Steam install found".
FLATPAK_STEAM="${HOME}/.var/app/com.valvesoftware.Steam/.local/share/Steam"
find_flatpak_steam() {
    [ -d "${FLATPAK_STEAM}/ubuntu12_32" ] && { echo "${FLATPAK_STEAM}"; return; }
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
# unset = decide automatically: build natively when the toolchain works, and
# fall back to a container only when it does not.
USE_CONTAINER=""
for arg in "$@"; do
    case "${arg}" in
        --uninstall|-u)     uninstall; exit 0 ;;
        --no-build)         NO_BUILD=1 ;;
        --container)        USE_CONTAINER=1 ;;
        --no-container)     USE_CONTAINER=0 ;;
        --with-millennium)  WITH_MILLENNIUM=1; MILLENNIUM_EXPLICIT=1 ;;
        --no-millennium)    WITH_MILLENNIUM=0; MILLENNIUM_EXPLICIT=1 ;;
        -h|--help)
            sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "unknown option '${arg}' (try --help)" ;;
    esac
done

# An ostree-booted system is an atomic image: Bazzite, Silverblue, Kinoite.
# dnf there is a shim that refuses and points at the documentation, so printing
# a dnf line would be telling someone to run a command their system has already
# decided to reject.
is_atomic() { [ -f /run/ostree-booted ]; }

# The full list, kept in one place because it has been wrong three times: the
# module needs zlib and the bootstrap needs X11 headers, and both were missing
# from every list here until a container build without them failed.
#
# No curl: the module dlopens libcurl at runtime and the build vendors the
# constants, so no curl development package is needed.
deps_hint() {
    if is_atomic; then
        warn "  This is an atomic image. Rather than layering a toolchain onto"
        warn "  it, install podman and re-run -- the build then happens in a"
        warn "  container and this machine needs no compiler at all:"
        warn ""
        warn "    rpm-ostree install podman   # or: sudo dnf install podman"
        warn ""
        warn "  Already have podman or docker? Re-running is all it takes."
        return
    fi
    warn "  Arch    : pacman -S --needed base-devel gcc-multilib cmake ninja \\"
    warn "                     lib32-glibc lib32-openssl lib32-zlib lib32-libxtst"
    warn "  Debian  : dpkg --add-architecture i386 && apt update &&"
    warn "            apt install build-essential g++-multilib cmake ninja-build \\"
    warn "                        libssl-dev:i386 zlib1g-dev:i386 libxtst-dev:i386"
    warn "  Fedora  : dnf install @development-tools gcc-c++ cmake ninja-build \\"
    warn "                        glibc-devel.i686 libstdc++-devel.i686 \\"
    warn "                        openssl-devel.i686 zlib-devel.i686 libXtst-devel.i686"
    warn ""
    warn "  Or install podman/docker and re-run: the build then happens in a"
    warn "  container and none of the above is needed."
}

# --- building in a container -------------------------------------------------
#
# Assembling a working 32-bit C++ toolchain is the single hardest part of
# installing this, and it is different on every distribution: multilib packages
# under four different names, headers split from libraries, and image-based
# systems where none of it can be installed at all. Four consecutive releases
# were spent on one user's machine finding missing pieces one report at a time.
#
# So this does not have to be solved on the host. The image below is pinned and
# known to build both architectures, the repository is the only thing mounted
# into it, and what comes out is what a native build would have produced.
CONTAINER_IMAGE="${SF_CONTAINER_IMAGE:-docker.io/library/ubuntu:24.04}"

container_engine() {
    command -v podman 2>/dev/null && return 0
    command -v docker 2>/dev/null && return 0
    return 1
}

build_in_container() {
    local engine
    engine="$(container_engine)" || {
        warn "No podman or docker found, so there is nowhere to build."
        return 1
    }
    say "Building in ${CONTAINER_IMAGE##*/} via ${engine##*/} (first run pulls the image)"

    # Root inside, because the packages have to be installed; the tree is handed
    # back on the way out, including when the build fails, so a failure never
    # leaves files the user cannot delete.
    #
    # :z on the mount is not optional on Fedora, Bazzite or anything else with
    # SELinux enforcing. Without it the bind mount keeps the host label, the
    # container cannot execute anything under it, and the build dies with a
    # bare "Permission denied" on a script that is plainly executable. It asks
    # SELinux to relabel the tree as container-shareable.
    #
    # bash ./tools/... rather than ./tools/..., so the build does not depend on
    # an executable bit surviving however the source arrived.
    "${engine}" run --rm \
        -v "${REPO_ROOT}:/src:z" \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        -e SF_BUILD_TYPE="${BUILD_TYPE}" \
        -w /src "${CONTAINER_IMAGE}" bash -uc '
            trap "chown -R \"${HOST_UID}:${HOST_GID}\" /src 2>/dev/null || true" EXIT
            export DEBIAN_FRONTEND=noninteractive
            dpkg --add-architecture i386
            apt-get update -qq
            apt-get install -y -qq --no-install-recommends \
                build-essential g++-multilib cmake ninja-build python3 file \
                binutils ca-certificates git \
                libssl-dev libssl-dev:i386 zlib1g-dev zlib1g-dev:i386 \
                libx11-dev libx11-dev:i386 libxtst-dev libxtst-dev:i386
            bash ./tools/build_linux.sh
        ' || return 1

    [ -f "${REPO_ROOT}/build/32/SteamFlipper.so" ] || {
        warn "The container finished but produced no 32-bit module."
        return 1
    }
    say "    container build finished"
    return 0
}

# --- prerequisites -----------------------------------------------------------
say "Checking prerequisites"
# Compile a snippet, and when it fails show the compiler's own first words.
# "32-bit OpenSSL headers are missing" is an inference; "openssl/evp.h: No such
# file or directory" is the fact, and it separates a package that was never
# installed from one that is there but cannot be used at -m32. Without it the
# only way to find out is to reproduce the probe by hand.
probe() {                            # probe <source> [compiler args...]
    local src="$1"; shift
    PROBE_ERR="$(printf '%s\n' "${src}" | g++ -m32 -x c++ - "$@" -o /dev/null 2>&1)"
}
# The diagnosis is at the END of a compiler's output, not the start: the first
# lines are the include chain that led there. head -2 showed
#   In file included from /usr/include/c++/14/string:38,
#                    from <stdin>:1:
# and cut off the "fatal error: ... No such file" that says what is actually
# wrong. So this finds the error line and shows that.
show_probe_err() {
    [ -n "${PROBE_ERR:-}" ] || return 0
    local shown
    shown="$(printf '%s\n' "${PROBE_ERR}" | grep -m2 -E 'error:|Error' || true)"
    [ -n "${shown}" ] || shown="$(printf '%s\n' "${PROBE_ERR}" | tail -3)"
    printf '%s\n' "${shown}" | while IFS= read -r line; do
        warn "    ${line}"
    done
}

# Can this machine build a 32-bit module?
#
# One question, asked once, answered without exiting: the caller decides what a
# "no" means, because it means "install the packages" on an ordinary system and
# "build in a container instead" everywhere else.
#
# Every probe compiles what it claims to test. That sounds obvious and was not:
# the C++ probe used to compile a bare main(), which reads no standard library
# header at all and passed on machines with no 32-bit libstdc++ whatsoever.
HOST_BUILD_PROBLEM=""
host_can_build() {
    local t missing=()
    for t in cmake ninja python3 gcc g++ readelf; do
        command -v "${t}" >/dev/null 2>&1 || missing+=("${t}")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        HOST_BUILD_PROBLEM="missing tools: ${missing[*]}"
        return 1
    fi

    if ! echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
        HOST_BUILD_PROBLEM="gcc cannot build 32-bit binaries"
        return 1
    fi

    # <string> rather than a bare main: a bare main reads no header, so it
    # cannot tell a working 32-bit C++ toolchain from a missing one.
    if ! probe '#include <string>
int main(){ std::string s; return s.size(); }'; then
        HOST_BUILD_PROBLEM="g++ cannot build 32-bit C++ (32-bit libstdc++ headers?)"
        return 1
    fi

    # <format> is C++20 and arrived in GCC 13. Older compilers get through every
    # check above and then fail deep in the build on a header nobody mentioned.
    if ! probe '#include <format>
int main(){ return (int)std::format("{}", 1).size(); }'; then
        HOST_BUILD_PROBLEM="the C++ compiler is too old (needs <format>, GCC 13+)"
        return 1
    fi

    if ! probe '#include <openssl/evp.h>
int main(){return 0;}' -lcrypto; then
        HOST_BUILD_PROBLEM="32-bit OpenSSL is missing or unusable"
        return 1
    fi

    # zlib is a real dependency of the module and was never checked or
    # documented, because it ships with the base system on the distribution
    # this was written on and is a separate package almost everywhere else.
    if ! probe '#include <zlib.h>
int main(){ return (int)zlibVersion()[0]; }' -lz; then
        HOST_BUILD_PROBLEM="32-bit zlib headers are missing"
        return 1
    fi

    # The libXtst bootstrap includes Xlib. Checked here because it is built with
    # the module now, and because a missing X11 header stops the install right
    # at the end, after the whole module has compiled.
    if ! probe '#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
int main(){ return 0; }'; then
        HOST_BUILD_PROBLEM="32-bit X11/XTest headers are missing (the bootstrap needs them)"
        return 1
    fi
    return 0
}

if [ "${NO_BUILD}" = "1" ]; then
    :                                   # installing what is already built
elif [ "${USE_CONTAINER}" = "1" ]; then
    build_in_container || die "the container build failed"
    NO_BUILD=1
elif host_can_build; then
    say "    toolchain OK"
elif [ "${USE_CONTAINER}" = "0" ]; then
    warn "${HOST_BUILD_PROBLEM}"
    show_probe_err
    deps_hint
    die "this machine cannot build a 32-bit module"
elif container_engine >/dev/null; then
    warn "This machine cannot build a 32-bit module: ${HOST_BUILD_PROBLEM}"
    show_probe_err
    warn "Falling back to a container, which needs no toolchain on the host."
    warn "Use --no-container to refuse this and fix the toolchain instead."
    build_in_container || {
        deps_hint
        die "the container build failed too"
    }
    NO_BUILD=1
elif [ -f /run/.containerenv ] || [ -f /.dockerenv ]; then
    # Already inside a container, so there is no engine here to fall back to.
    # The answer is not to fix this container: it is to leave it, because the
    # host can now do the whole job with one command.
    warn "${HOST_BUILD_PROBLEM}"
    show_probe_err
    warn ""
    warn "This is running inside a container, and containers rarely have podman"
    warn "or docker in them, so there is nothing to fall back to here."
    warn ""
    warn "You no longer need this container. Since 1.2.0 the installer builds in"
    warn "one by itself, so run it on the host instead:"
    warn ""
    warn "    exit                       # leave this container"
    warn "    cd ${REPO_ROOT##*/} && ./tools/install_linux.sh"
    warn ""
    warn "The host needs podman or docker and nothing else. To stay in here"
    warn "instead, install the packages for this container's distribution:"
    deps_hint
    die "this machine cannot build a 32-bit module"
else
    warn "${HOST_BUILD_PROBLEM}"
    show_probe_err
    warn ""
    warn "Either install the packages below, or install podman or docker and"
    warn "run this again -- it will then build in a container and need no"
    warn "toolchain here at all."
    deps_hint
    die "this machine cannot build a 32-bit module"
fi

if ! STEAM_DIR="$(find_steam)"; then
    if find_flatpak_steam >/dev/null; then
        warn "Found Steam installed as a Flatpak:"
        warn "  ${FLATPAK_STEAM}"
        warn ""
        warn "That is not supported. SteamFlipper gets into the client by replacing"
        warn "a library next to the Steam binary, and in a Flatpak that library"
        warn "belongs to the read-only runtime. Install Steam natively -- on an"
        warn "atomic image, the native package or the one your image already ships"
        warn "in game mode -- and run this again."
        die  "Flatpak Steam is not supported"
    fi
    die "no Steam install found (looked for ubuntu12_32/); set SF_STEAM_DIR"
fi
say "    Steam at ${STEAM_DIR}"

if pgrep -x steam >/dev/null 2>&1; then
    die "Steam is running — close it first: steam -shutdown"
fi

# That check only sees processes this namespace can see. Run from a container
# -- which is how an atomic image gets a toolchain -- it will happily report
# nothing while Steam is up on the host, and installing over a running client
# is the most common way this goes wrong.
if [ -f /run/.containerenv ] || [ -f /.dockerenv ]; then
    warn "Running inside a container: cannot tell whether Steam is up on the host."
    warn "Make sure you ran 'steam -shutdown' there before continuing."
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
PREBUILT_BOOT="${REPO_ROOT}/build/32/libXtst.so.6"
if [ -f "${PREBUILT_BOOT}" ]; then
    # Built alongside the module, which is what lets a machine with no working
    # 32-bit toolchain install artifacts built elsewhere -- in a container, say.
    cp -a "${PREBUILT_BOOT}" "${TMP_BOOT}/libXtst.so.6"
else
    gcc -m32 -shared -fPIC -O2 -o "${TMP_BOOT}/libXtst.so.6" "${BOOTSTRAP_SRC}" -ldl \
        || die "bootstrap failed to build"
fi

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
# The module regenerates these itself after a Steam update, which needs the
# generator to still be somewhere it can find. Beside its own data rather than
# in the source tree: an install outlives the checkout it came from, and a Deck
# updates Steam often enough that "re-run the installer" is the step that never
# happens.
install -Dm755 "${REPO_ROOT}/tools/gen_linux_patterns.py" \
               "${STEAM_DIR}/ubuntu12_32/steamflipper/gen_linux_patterns.py"

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

# --- client UI ---------------------------------------------------------------
# The LUAFlipper tab is injected into Steam's CEF frontend at runtime, so the
# assets live on disk rather than being compiled in; the module reads them from
# here on every launch and a reinstall is enough to pick up UI changes.
say "Installing client UI"
mkdir -p "${STEAM_DIR}/steamflipper/ui"
for f in luaflipper.js luaflipper.pages.js luaflipper.store.js luaflipper.popup.js luaflipper.css; do
    if [ -f "${REPO_ROOT}/plugin/luaflipper/public/${f}" ]; then
        install -Dm644 "${REPO_ROOT}/plugin/luaflipper/public/${f}" \
                       "${STEAM_DIR}/steamflipper/ui/${f}"
    else
        warn "    missing ${f}; the UI may not load"
    fi
done

# Steam only opens the CEF debugger, the sole channel into the frontend, when
# this marker exists. It is read at client startup, so creating it here means it
# is already in place for the next launch.
if [ ! -e "${STEAM_DIR}/.cef-enable-remote-debugging" ]; then
    : > "${STEAM_DIR}/.cef-enable-remote-debugging"
    say "    enabled CEF remote debugging (needed by the UI)"
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
