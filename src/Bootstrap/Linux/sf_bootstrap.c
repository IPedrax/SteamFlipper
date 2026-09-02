/**
 * ==================================================
 *   _____ _ _ _             _
 *  |     |_| | |___ ___ ___|_|_ _ _____
 *  | | | | | | | -_|   |   | | | |     |
 *  |_|_|_|_|_|_|___|_|_|_|_|_|___|_|_|_|
 *
 * ==================================================
 *
 * Copyright (c) 2026 Project Millennium
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef __linux__
#include "shared.h"
#include <stdlib.h>

static void* h_millennium = NULL;
static int b_has_loaded_millennium = 0;

typedef int (*start_millennium_t)(void);
typedef int (*stop_millennium_t)(void);

#ifdef MILLENNIUM_RUNTIME_PATH
static const char* k_millennium_path = MILLENNIUM_RUNTIME_PATH;
#else
static const char* get_millennium_library_path(void)
{
    static char path_buffer[PATH_MAX];
    static int initialized = 0;

    if (!initialized) {
        const char* envPath = getenv("MILLENNIUM_RUNTIME_PATH");
        if (envPath) {
            strncpy(path_buffer, envPath, PATH_MAX - 1);
            path_buffer[PATH_MAX - 1] = '\0';
        } else {
            strncpy(path_buffer, "/usr/lib/millennium/libmillennium_x86.so", PATH_MAX - 1);
            path_buffer[PATH_MAX - 1] = '\0';
        }
        initialized = 1;
    }
    return path_buffer;
}
#define k_millennium_path (get_millennium_library_path())
#endif

static int load_and_start_millennium(void)
{

    h_millennium = dlopen_or_log(k_millennium_path, RTLD_LAZY | RTLD_GLOBAL);
    if (!h_millennium) return 0;

    start_millennium_t fn_start = (start_millennium_t)dlsym_or_log(h_millennium, "StartMillennium");
    if (!fn_start) {
        dlclose(h_millennium);
        h_millennium = NULL;
        return 0;
    }

    int result = fn_start();
    if (result < 0) {
        LOG_ERROR("Failed to start Millennium: %d", result);
        dlclose(h_millennium);
        h_millennium = NULL;
        return 0;
    }

    return 1;
}

static void stop_and_unload_millennium(void)
{
    if (!h_millennium) {
        LOG_ERROR("Millennium library is not loaded.");
        return;
    }

    stop_millennium_t fn_stop = (stop_millennium_t)dlsym_or_log(h_millennium, "StopMillennium");
    if (fn_stop) {
        int result = fn_stop();
        if (result < 0) LOG_ERROR("Failed to stop Millennium: %d", result);
    }

    dlclose(h_millennium);
    h_millennium = NULL;
    b_has_loaded_millennium = 0;
}

static void setup_hooks(void)
{
    const char* p = get_process_path_parent();
    if (!p) {
        LOG_ERROR("Failed to retrieve current directory.");
        return;
    }

    char lbxtst_path[PATH_MAX];
    get_steam_lib_path(lbxtst_path, p, "i386-linux-gnu", "libXtst.so.6");

    if (access(lbxtst_path, F_OK) == -1) {
        LOG_ERROR("Pinned libXtst does not exist at: %s", lbxtst_path);
        return;
    }

    h_xtst = dlopen_or_log(lbxtst_path, RTLD_LAZY | RTLD_GLOBAL);
}

static int is_steam_process(void)
{
    char* p = get_process_path();
    if (!p) return 0;

    char rp[PATH_MAX];
    if (!realpath(p, rp)) return 0;

    /*
     * Match on the shape of the path, not on a fixed install location.
     *
     * This previously compared against "$HOME/.steam/steam/ubuntu12_32/steam".
     * That only works where the ~/.steam/steam compatibility symlink exists and
     * points at the real library — not true for Flatpak Steam, ~/.steam/root
     * layouts, or a relocated STEAMROOT. When realpath() failed on that path
     * this returned 0, so the module silently never loaded and the install
     * looked like a no-op with nothing in the log to explain it.
     *
     * The 32-bit client is always <anything>/ubuntu12_32/steam, so a suffix
     * match is both more permissive and more precise.
     */
    static const char k_suffix[] = "/ubuntu12_32/steam";
    const size_t len = strlen(rp);
    const size_t slen = sizeof(k_suffix) - 1;

    return len >= slen && strcmp(rp + len - slen, k_suffix) == 0;
}

static void libmillennium_bootstrap_init(void) __attribute__((constructor));
static void libmillennium_bootstrap_cleanup(void) __attribute__((destructor));

/* ------------------------------------------------------------------------- *
 * SteamFlipper co-load.
 *
 * SteamFlipper previously injected via LD_PRELOAD, which the dynamic loader propagates
 * to every process Steam spawns — steamwebhelper, the runtime launcher, Proton
 * and the games. Loading it here instead means one injection point for both
 * mods: this proxy is already resolved only inside the Steam client, so it
 * lands exactly where its hooks apply and nowhere else.
 *
 * RTLD_LOCAL on purpose: SteamFlipper statically links protobuf/lua/spdlog, and putting
 * that surface in the global scope would interpose on Steam's own copies.
 * ------------------------------------------------------------------------- */
static void* h_sf = NULL;

static const char* get_sf_library_path(void)
{
    static char sf_path[PATH_MAX];
    static int sf_initialized = 0;

    if (!sf_initialized) {
        const char* env_path = getenv("SF_RUNTIME_PATH");
        if (env_path) {
            strncpy(sf_path, env_path, PATH_MAX - 1);
        } else {
            const char* home = getenv("HOME");
            snprintf(sf_path, PATH_MAX, "%s/.local/lib/steamflipper/32/SteamFlipper.so",
                     home ? home : "");
        }
        sf_path[PATH_MAX - 1] = '\0';
        sf_initialized = 1;
    }
    return sf_path;
}

static void load_steamflipper(void)
{
    const char* path = get_sf_library_path();

    if (getenv("SF_DISABLE")) {
        LOG_INFO("SF_DISABLE set; skipping SteamFlipper");
        return;
    }
    if (access(path, F_OK) == -1) {
        LOG_INFO("SteamFlipper not installed at %s; skipping", path);
        return;
    }

    h_sf = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!h_sf) {
        LOG_ERROR("Failed to load SteamFlipper: %s", dlerror());
        return;
    }
    LOG_INFO("Loaded SteamFlipper from %s", path);
}

static void proxy_at_exit_handler(void)
{
    LOG_INFO("at_exit: invoking stop_and_unload_millennium()");
    if (b_has_loaded_millennium) {
        stop_and_unload_millennium();
    }
}

static void libmillennium_bootstrap_init(void)
{
    if (!is_steam_process()) {
        LOG_INFO("Skipping Millennium setup for non-Steam process. Process path: %s", get_process_path());
        return;
    }

    /*
     * SteamFlipper-only by default; Millennium is opt-in via MILLENNIUM_ENABLE=1.
     *
     * This proxy replaces libXtst.so.6, so it is the injection point for both
     * mods. Defaulting to SteamFlipper-only skips Millennium's CreateSimpleProcess hook
     * and core. XTest forwarding is established above, before this branch, so
     * it is intact in either mode.
     *
     * The default is inverted from upstream deliberately: Millennium's 64-bit
     * bootstrap currently prevents steamwebhelper from starting on this
     * install, and that hook is reached from here. Flip MILLENNIUM_ENABLE once
     * Millennium has been reinstalled through its own installer.
     */
    /*
     * Open the real libXtst first, in BOTH modes. We are standing in for
     * libXtst.so.6, so the five XTest entry points we re-export have to reach
     * the genuine implementation; with h_xtst NULL the HOOK_FUNC stubs return 0
     * for everything, which makes XTestQueryExtension report the extension as
     * unavailable and silently kills Steam's synthetic input (Big Picture
     * virtual keyboard, controller-driven cursor). This used to sit inside the
     * Millennium-only branch, so the default SteamFlipper-only path never ran it.
     */
    LOG_INFO("Setting up proxy hooks...");
    setup_hooks();

    /*
     * Millennium is NOT co-loaded from here, and MILLENNIUM_ENABLE is retired.
     *
     * Loading it via dlopen from this constructor reliably kills steamwebhelper:
     * Millennium hooks main() and rewrites LD_PRELOAD for the steamwebhelper
     * child (swapping its 32-bit core for the hhx64 bootstrap). Both depend on
     * it having been mapped by the dynamic loader BEFORE Steam initialises, which
     * a dlopen from here cannot reproduce -- priming LD_PRELOAD first was tried
     * and does not help, because it is load order, not the variable, that matters.
     *
     * The two run together happily using their native, independent injection
     * paths: SteamFlipper through this libXtst proxy, Millennium through its own
     * LD_PRELOAD. Verified: 12 steamwebhelper processes with both mods live.
     *
     *     LD_PRELOAD=/usr/lib/millennium/libmillennium_x86.so steam
     */
    if (getenv("MILLENNIUM_ENABLE")) {
        LOG_INFO("MILLENNIUM_ENABLE is retired: co-loading Millennium from this "
                 "proxy prevents steamwebhelper from starting. Launch Steam with "
                 "LD_PRELOAD=/usr/lib/millennium/libmillennium_x86.so instead; "
                 "SteamFlipper still loads from here.");
    }
    load_steamflipper();
    return;

    b_has_loaded_millennium = 1;

    LOG_INFO("Bootstrap library loaded successfully. Using Millennium library at: %s", k_millennium_path);
    if (!load_and_start_millennium()) {
        LOG_ERROR("Failed to load Millennium...");
    } else {
        LOG_INFO("Started Millennium...");

        if (atexit(proxy_at_exit_handler) != 0) {
            LOG_ERROR("Failed to register atexit handler for Millennium cleanup");
        }
    }

    /*
     * Loaded last, and deliberately regardless of Millennium's outcome: the two
     * mods are independent, so a broken Millennium install must not take
     * SteamFlipper down with it (nor the reverse). Its own constructor re-checks that the
     * host process is the Steam client before doing any work.
     */
    load_steamflipper();
}

static void libmillennium_bootstrap_cleanup(void)
{
    LOG_INFO("Unloading Millennium library...");

    if (h_xtst) dlclose(h_xtst);
    if (!b_has_loaded_millennium) return;

    stop_and_unload_millennium();
}
#endif
