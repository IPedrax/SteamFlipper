#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/CloudSaves/CloudSaves.h"
#include "Utils/LuaFlipperUI/LuaFlipperUI.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Tokeer/TokeerBridge.h"
#include "Utils/Update/AppUpdater.h"
#include "SFPlatform/include/Dialog.h"
#include "SFPlatform/include/DynamicLibrary.h"
#include "SFPlatform/include/Thread.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#include <cstring>
#endif

#ifndef _WIN32
// Steam keeps the client core in an arch-specific subdirectory whose name has
// drifted across releases (ubuntu12_*, linux*, steamrt*), and no single one is
// present on every install. Probe only the dirs matching our own word size — a
// 64-bit process cannot dlopen a 32-bit steamclient.so.
static std::string ResolveSteamModule(const std::string& root, const char* file)
{
#if defined(__i386__)
    static const char* const kDirs[] = { "ubuntu12_32", "linux32", "steamrt32" };
#else
    static const char* const kDirs[] = { "linux64", "steamrt64", "ubuntu12_64" };
#endif
    for (const char* dir : kDirs) {
        std::string candidate = root + "/" + dir + "/" + file;
        if (access(candidate.c_str(), F_OK) == 0) {
            return candidate;
        }
    }
    return root + "/" + file;
}
#endif

// prepare key runtime paths.
bool InitializeSteamComponents()
{
    const std::string steamInstallPath = SFPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    if (steamInstallPath.empty()) {
        return false;
    }

#ifdef _WIN32
    snprintf(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    snprintf(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    snprintf(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    snprintf(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion.dll", SteamInstallPath);
    snprintf(LuaDir,          kRuntimePathCapacity, "%s\\config\\stplug-in",  SteamInstallPath);
    snprintf(ConfigPath,      kRuntimePathCapacity, "%s\\steamflipper.toml", SteamInstallPath);
#else
    snprintf(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    snprintf(SteamclientPath, kRuntimePathCapacity, "%s",
             ResolveSteamModule(steamInstallPath, "steamclient.so").c_str());
    snprintf(SteamUIPath,     kRuntimePathCapacity, "%s",
             ResolveSteamModule(steamInstallPath, "steamui.so").c_str());

    snprintf(DiversionPath,   kRuntimePathCapacity, "%s/bin/diversion.so", SteamInstallPath);
    snprintf(LuaDir,          kRuntimePathCapacity, "%s/config/stplug-in", SteamInstallPath);
    snprintf(ConfigPath,      kRuntimePathCapacity, "%s/steamflipper.toml", SteamInstallPath);
#endif
    
    client_hModule = SFPlatform::DynamicLibrary::Load(SteamclientPath);
    if (!client_hModule) {
        LOG_ERROR("Load steamclient failed: {} (err={})",
                  SteamclientPath, SFPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    LOG_INFO("Loaded steamclient from {}", SteamclientPath);
    
    ui_hModule = SFPlatform::DynamicLibrary::Load(SteamUIPath);
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui: err={}", SFPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread.
static uint32_t InitThread(SFPlatform::DynamicLibrary::ModuleHandle selfModule) {
    Log::Init(selfModule);
    LOG_INFO("SteamFlipper init thread started");

    if (!InitializeSteamComponents()) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient and steamui.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs =
        LuaConfig::MergeWatchDirs(Config::GetLuaPaths(), std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamUI::CoreHook();
    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect)
    CloudRedirectHost::Initialize(SteamInstallPath);

    // Native cloud saves. Replaces CloudRedirect for the Cloud.* RPCs; its
    // Linux build exports no host API and its own hooks do not match this
    // client, so the RPCs are answered in-process instead.
    CloudSaves::Initialize(SteamInstallPath);

    // In-client LUAFlipper UI. Injects over CEF's debugger, so it is started
    // last: CEF is not up yet at this point and the thread polls for it.
    LuaFlipperUI::Initialize(SteamInstallPath);

#ifdef _WIN32
    // Register the bst:// URI scheme so the website can drive code redemption
    TokeerBridge::RegisterUriScheme(std::string(SteamInstallPath) + "\\SteamFlipper.dll");
#endif

    // Optional self-update check, always on its own detached thread: it makes a
    // network call, and init must not wait on one.
    //
    // The two platforms update different things. Windows fetches a published
    // DLL and stages it. Linux has no published artifact — the module is built
    // locally by tools/install_linux.sh — so "an update" is a commit on the
    // branch this was built from, and all this does is say so in the log. It
    // never pulls and never shows a dialog: the installer refuses to run while
    // Steam is up, which is precisely when this code is alive, so anything
    // beyond reporting would be a prompt nobody can act on yet. The Config page
    // owns the button that acts on it.
    if (Config::GetUpdateEnabled()) {
        SFPlatform::Thread::StartDetached([] () -> uint32_t {
#ifdef _WIN32
            const std::string self = std::string(SteamInstallPath) + "\\SteamFlipper.dll";
            AppUpdater::CleanupStagedBackup(self);

            const AppUpdater::CheckResult upd = AppUpdater::Check();
            if (!upd.updateAvailable) return 0;
            if (!AppUpdater::DownloadAndStage(upd, self)) return 0;

            const bool restart = SFPlatform::Dialog::ShowConfirm(
                "SteamFlipper Updated!",
                upd.oldVersion + " -> " + upd.newVersion +
                "\n\nRestart Steam now to apply?");
            if (restart) AppUpdater::RestartSteam();
#else
            // Which file is actually mapped, rather than which one the
            // installer would have written: they differ during development, and
            // this is the line that says which one a rebuild has to replace.
            LOG_INFO("SteamFlipper module: {}", AppUpdater::SelfPath());

            const AppUpdater::SourceCheck s = AppUpdater::CheckSource();
            if (!s.error.empty())
                LOG_WARN("AppUpdater: update check failed: {}", s.error);
            else if (!s.reason.empty())
                LOG_INFO("AppUpdater: no update check: {}", s.reason);
            else if (s.behind)
                LOG_INFO("AppUpdater: {} has moved to {} (\"{}\"); this build is {}. "
                         "Pull and run tools/install_linux.sh with Steam closed.",
                         s.branch, s.remote, s.message, s.sha);
#endif
            return 0;
        });
    }

    LOG_INFO("SteamFlipper init complete");
    return 0;
}

// True only when the host process is the Steam client itself.
static bool IsSteamHost()
{
#ifdef _WIN32
    char exePath[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return false;
    const char* name = strrchr(exePath, '\\');
    name = name ? name + 1 : exePath;
    return _stricmp(name, "steam.exe") == 0;
#else
    // LD_PRELOAD is inherited by every process Steam spawns — steamwebhelper,
    // the runtime launcher, reaper, Proton and the games themselves — so the
    // constructor fires far more often than DllMain ever did. Without this gate
    // each child would load steamclient, install detours and start watchers off
    // its own (wrong) cwd.
    return SFPlatform::DynamicLibrary::GetMainExecutablePath().filename() == "steam";
#endif
}

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        if (!IsSteamHost())
            return TRUE;
        SFPlatform::Thread::StartDetached([module = reinterpret_cast<SFPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH && IsSteamHost())
    {
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
        CloudSaves::Shutdown();
    }

    return TRUE;
}
#else
__attribute__((constructor)) static void OnLoad() {
    if (!IsSteamHost())
        return;   // inherited LD_PRELOAD in a child process — nothing to do here
    SFPlatform::Thread::StartDetached([] {
        return InitThread(nullptr);
    });
}

__attribute__((destructor)) static void OnUnload() {
    if (!IsSteamHost())
        return;
    ConfigFileWatcher::Stop();
    LuaFileWatcher::Stop();
    SteamUI::CoreUnhook();
    SteamClient::CoreUnhook();
    CloudRedirectHost::Shutdown();
    // Nothing else called this, so the loopback endpoint stayed up until the
    // process died. The join that keeps ~thread() from terminating is
    // registered in CloudSaves itself, because .fini_array runs too late for
    // it; this is the orderly shutdown, not the safety net.
    CloudSaves::Shutdown();
}
#endif
