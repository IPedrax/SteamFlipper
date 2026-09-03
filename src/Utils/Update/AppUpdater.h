#pragma once

#include <string>

namespace AppUpdater {

    struct CheckResult {
        bool updateAvailable = false;
        std::string oldVersion;   // version baked into the running DLL
        std::string newVersion;   // version named by latest.toml
        std::string dllRelPath;   // branch-relative path to the new DLL
        std::string sha256;       // expected SHA-256 of the new DLL
    };

    // Read the mirror pointer (steamflipper/latest.toml) and decide whether the
    // published build differs from the running one. Never throws; returns
    // updateAvailable = false on any failure.
    CheckResult Check();

    // Download the new DLL, validate it (size, MZ header, SHA-256), and stage it next
    // to Steam via rename-then-write. The current (loaded) DLL is renamed to
    // "<name>.old"; the new bytes are written at the canonical path and load on the
    // next Steam start. Returns false (leaving the live DLL untouched) on any failure.
    bool DownloadAndStage(const CheckResult& result, const std::string& selfDllPath);

    // Best-effort removal of a "<name>.old" left by a previous staged update. Succeeds
    // only once the old image is no longer loaded (i.e. after a restart), so it is safe
    // to call unconditionally on startup.
    void CleanupStagedBackup(const std::string& selfDllPath);

    // Launch a detached, hidden helper that gracefully restarts Steam so a staged
    // update takes effect immediately. No-op-safe: if the helper can't launch, the
    // update still applies on the user's next manual start.
    void RestartSteam();

#if defined(__linux__)

    /*
     * Linux updates the source tree, not the binary.
     *
     * There is no published .so to fetch: the module is built locally by
     * tools/install_linux.sh, so "up to date" means "the checkout matches the
     * branch head", and applying an update means fast-forwarding that checkout.
     * The rebuild stays the user's step -- it needs the 32-bit toolchain, and
     * the installer refuses to run while Steam is up, which is exactly when
     * this code is alive.
     */

    // Absolute path of the running module, from dladdr() on an address inside
    // it. Falls back to $SF_RUNTIME_PATH and then the installer's own path;
    // logs which of the three answered. Empty only if all three fail.
    std::string SelfPath();

    struct SourceCheck {
        std::string sha;      // commit baked in at build time, or "unknown"
        std::string branch;   // branch baked in at build time, or "unknown"
        std::string remote;   // short sha at the branch head, when reachable
        std::string message;  // first line of that commit's message
        bool behind = false;
        std::string reason;   // set when no comparison was possible
        std::string error;    // set when the lookup itself failed
    };

    // Compare the baked-in commit against the branch head on GitHub. Network
    // only, touches nothing on disk, and never throws. An "unknown" baked sha
    // is reported through `reason` rather than treated as out of date.
    SourceCheck CheckSource();

    struct PullResult {
        bool ok = false;
        std::string sha;      // HEAD of the checkout after the attempt
        // One of: pulled, already-current, dirty-tree, not-fast-forward,
        // no-repo, no-git, network-failed, failed.
        std::string status;
        std::string repo;     // the checkout that was operated on, when known
        std::string error;
    };

    // git -C <[update].repo> pull --ff-only. Refuses on a dirty tree and never
    // stashes, resets or forces: local work outranks an update. Blocks on the
    // network, so it belongs on a worker thread.
    PullResult PullSource();

#endif // __linux__

} // namespace AppUpdater
