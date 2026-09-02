#include "SteamDiagnostics.h"
#include "SteamFlipperBuildInfo.h"
#include "SFPlatform/include/Dialog.h"
#include "SFPlatform/include/DynamicLibrary.h"
#include "SFPlatform/include/Hash.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <cstdint>
#include <thread>
#include <utility>

namespace SteamDiagnostics {

namespace {

    struct Snapshot {
        std::string openSteamToolVersion = STEAMFLIPPER_VERSION;
        std::string buildID = "(unavailable)";
        std::string steamclientPath;
        std::string steamclientSha256 = "(unavailable)";
        std::string steamUIPath;
        std::string steamUISha256 = "(unavailable)";
    };

    Snapshot g_snapshot;

    static std::string DetectSteamBuildID()
    {
        using GetBootstrapperVersion_t = int64_t (*)();

        const auto steam = SFPlatform::DynamicLibrary::GetLoaded("steam.exe");
        if (!steam) {
            LOG_WARN("SteamDiagnostics: steam.exe module not loaded; build id unavailable");
            return "(unavailable)";
        }

        const auto getBootstrapperVersion =
            reinterpret_cast<GetBootstrapperVersion_t>(
                SFPlatform::DynamicLibrary::GetSymbol(steam, "GetBootstrapperVersion"));
        if (!getBootstrapperVersion) {
            LOG_WARN("SteamDiagnostics: steam.exe!GetBootstrapperVersion not exported");
            return "(unavailable)";
        }

        return std::to_string(getBootstrapperVersion());
    }

    static std::string HashOrUnavailable(const std::string& path)
    {
        std::string sha256 = SFPlatform::Hash::Sha256OfFile(path);
        return sha256.empty() ? "(unavailable)" : std::move(sha256);
    }

    static std::string AppendSnapshot(std::string message)
    {
        message +=
            "\n\nSteam diagnostics:\n"
            "  SteamFlipper version: " + g_snapshot.openSteamToolVersion + "\n"
            "  Build ID:              " + g_snapshot.buildID + "\n"
#ifdef _WIN32
            "  steamclient64.dll SHA: " + g_snapshot.steamclientSha256 + "\n"
            "  steamui.dll SHA:       " + g_snapshot.steamUISha256;
#else
            // The Linux client loads .so modules; naming the Windows DLLs
            // here sends people looking for files that do not exist.
            "  steamclient.so SHA: " + g_snapshot.steamclientSha256 + "\n"
            "  steamui.so SHA:     " + g_snapshot.steamUISha256;
#endif
        return message;
    }

} // namespace

void Initialize(const std::string& steamclientPath,
                const std::string& steamUIPath)
{
    g_snapshot.buildID = DetectSteamBuildID();
    g_snapshot.steamclientPath = steamclientPath;
    g_snapshot.steamclientSha256 = HashOrUnavailable(steamclientPath);
    g_snapshot.steamUIPath = steamUIPath;
    g_snapshot.steamUISha256 = HashOrUnavailable(steamUIPath);

    LOG_INFO("SteamDiagnostics: ost.version={} build={} steamclient64.sha256={} steamui.sha256={}",
             g_snapshot.openSteamToolVersion,
             g_snapshot.buildID,
             g_snapshot.steamclientSha256,
             g_snapshot.steamUISha256);
}

std::string Sha256Of(const std::string& path)
{
    if (path == g_snapshot.steamclientPath)
        return g_snapshot.steamclientSha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamclientSha256;

    if (path == g_snapshot.steamUIPath)
        return g_snapshot.steamUISha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamUISha256;

    return SFPlatform::Hash::Sha256OfFile(path);
}

void ShowWarning(std::string title, std::string message)
{
    // Always record it — suppressing the dialog must not cost the diagnosis.
    LOG_WARN("{}: {}", title, message);

    if (!Config::GetDiagnosticsPopups()) {
        return;
    }

    std::thread([title = std::move(title),
                 message = AppendSnapshot(std::move(message))]() {
        SFPlatform::Dialog::ShowWarning(title, message);
    }).detach();
}

} // namespace SteamDiagnostics
