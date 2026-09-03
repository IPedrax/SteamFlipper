#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "Steam/Types.h"

namespace Config {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    struct ManifestTimeouts {
        uint32_t resolve = 5000;
        uint32_t connect = 5000;
        uint32_t send    = 10000;
        uint32_t recv    = 10000;
    };

    // [[inject]] entry: a DLL loaded into a matching game process at the IPC handshake.
    struct InjectDll {
        std::string                 path;        // resolved absolute path
        std::string                 whenCmdline; // substring required in the game command line
        std::unordered_set<AppId_t> whenAppids;  // appids this entry applies to
        bool                        allGames = false;  // false: only Lua-unlocked games
    };

    struct CloudSettings {
        bool enabled = false;
        std::string library;
    };

    struct LoadResult {
        bool applied = false;
        bool luaPathsChanged = false;
    };

    LoadResult Load(const std::string& configPath);

    ManifestTimeouts GetManifestTimeouts();
    LogLevel GetLogLevel();
    std::string GetLogDir();
    std::vector<std::string> GetLuaPaths();
    std::vector<std::string> GetRemoteUrlTemplates();
    CloudSettings GetCloudSettings();
    bool GetStatsEnableApi();
    bool GetUpdateEnabled();
    std::string GetUpdateRepo();

    // [fixes].token - a lua.tools bearer token. The fix catalog is readable
    // without one; downloading a fix is not, and the token belongs to the
    // user's own account with its own daily cap, so it is theirs to supply and
    // is never shipped or defaulted.
    std::string GetFixesToken();

    // [hubcap].key - the user's own hubcapmanifest.com API key, "smm_" plus 96
    // hex. Hubcap is the one manifest source that takes no proxy: the key is
    // the user's, downloads count against their own daily limit, and nothing
    // here mints or validates one.
    std::string GetHubcapKey();

    // [sources].order - the manifest sources to try, most preferred first.
    // Empty means the built-in order. Names not recognised are ignored, and
    // any source left out of the list is still tried, just last: a typo should
    // reorder nothing rather than silently disable a source.
    std::vector<std::string> GetSourceOrder();

    // [ui].enabled - the in-client LUAFlipper UI. Default true.
    bool GetUiEnabled();

    // [ui].popup_menu - draw the nav dropdown as a Steam popup window. Default
    // true. Off falls back to the in-page menu, which cannot float over a
    // browser view and so moves the page aside, but has no window of its own
    // and none of the pointer-tracking that goes with one.
    bool GetUiPopupMenu();
    bool GetDiagnosticsPopups();

    // [manifest] — provider selection lives in ManifestClient (table-driven).
    inline uint32_t manifestTimeoutResolve = 5000;
    inline uint32_t manifestTimeoutConnect = 5000;
    inline uint32_t manifestTimeoutSend    = 10000;
    inline uint32_t manifestTimeoutRecv    = 10000;

    // [log]
    inline LogLevel logLevel = LogLevel::Debug;

    // derived from configPath: <steam>/steamflipper/
    inline std::string logDir;

    // [lua]
    inline std::vector<std::string> luaPaths;

    // [remote] — one or more mirror templates, tried in order. Empty = built-in defaults.
    inline std::vector<std::string> remoteUrlTemplates;

    // [stats]
    inline bool statsEnableApi = true;

    // [update] - self-update check on startup (staged for next Steam launch).
    inline bool updateEnabled = true;

    // [update].repo - the SteamFlipper source checkout this install was built
    // from. Not derivable: the installed module sits under ~/.local/lib and
    // carries no pointer back to the tree, so an unset value means "unknown"
    // and the updater refuses to pull rather than guessing a path.
    inline std::string updateRepo;

    // [fixes].token - see GetFixesToken above. Empty means "not configured",
    // which the Fixes page reports rather than papering over: a download that
    // 401s is not a network problem and should not be described as one.
    inline std::string fixesToken;

    // [hubcap].key - see GetHubcapKey above. Empty means the Sadie (Hubcap)
    // source reports "needs key" rather than pretending to be unavailable.
    inline std::string hubcapKey;

    // [sources].order - see GetSourceOrder above.
    inline std::vector<std::string> sourceOrder;

    // [diagnostics] — when false, SteamDiagnostics::ShowWarning stays silent.
    // Signature/IPC specs are published per steamclient hash, and none exist
    // yet for the Linux binaries, so every launch would otherwise raise the
    // same three unactionable popups.
    inline bool diagnosticsPopups = true;

    // [ui] - the in-client LUAFlipper tab, injected over Steam's CEF debugger.
    inline bool uiEnabled = true;
    inline bool uiPopupMenu = true;

    // [[inject]] - optional DLL injection into matching game processes.
    inline std::vector<InjectDll> injectDlls;

    // [cloud] - optional Steam Cloud save redirection via CloudRedirect.
    inline bool cloudEnabled = false;
    inline std::string cloudLibrary;

}
