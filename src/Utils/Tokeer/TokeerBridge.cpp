#include "TokeerBridge.h"

#include "SFPlatform/include/Dialog.h"
#include "SFPlatform/include/Http.h"
#include "SFPlatform/include/Numbers.h"
#include "SFPlatform/include/Process.h"
#include "SFPlatform/include/SteamCredentialStore.h"
#include "Utils/Logging/Log.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Code-server base URL, baked in at build time.
#ifndef SF_TOKEER_URL
#define SF_TOKEER_URL "https://luastools.xyz"
#endif

namespace TokeerBridge {

namespace {

    constexpr const char* kUriScheme = "bst";

    int HexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::optional<std::vector<uint8_t>> HexToBytes(std::string_view hex) {
        if (hex.empty() || (hex.size() % 2) != 0) return std::nullopt;
        std::vector<uint8_t> out;
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            const int hi = HexNibble(hex[i]);
            const int lo = HexNibble(hex[i + 1]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return out;
    }

    // Minimal "key":"value" string extraction from the server's flat JSON reply.
    bool JsonString(std::string_view body, std::string_view key, std::string& out) {
        const std::string needle = "\"" + std::string(key) + "\"";
        size_t k = body.find(needle);
        if (k == std::string_view::npos) return false;
        size_t colon = body.find(':', k + needle.size());
        if (colon == std::string_view::npos) return false;
        size_t q1 = body.find('"', colon + 1);
        if (q1 == std::string_view::npos) return false;
        size_t delim = body.find_first_of(",}", colon + 1);
        if (delim != std::string_view::npos && q1 > delim) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        out = std::string(body.substr(q1 + 1, q2 - q1 - 1));
        return true;
    }

    bool JsonTrue(std::string_view body, std::string_view key) {
        const std::string needle = "\"" + std::string(key) + "\"";
        size_t k = body.find(needle);
        if (k == std::string_view::npos) return false;
        size_t colon = body.find(':', k + needle.size());
        if (colon == std::string_view::npos) return false;
        return body.find("true", colon + 1) == body.find_first_not_of(" \t", colon + 1);
    }

    std::string TokeerBaseUrl() {
        std::string url = SF_TOKEER_URL;
        while (!url.empty() && url.back() == '/') url.pop_back();
        return url;
    }

    void Warn(const std::string& title, const std::string& msg) {
        SFPlatform::Dialog::ShowWarning(title, msg);
    }

} // namespace

void Redeem(const std::string& code) {
    namespace CS = SFPlatform::SteamCredentialStore;

    const std::string url = TokeerBaseUrl() + "/drm/redeem";
    const std::string reqBody = "{\"code\":\"" + code + "\"}";

    SFPlatform::Http::Result r = SFPlatform::Http::Execute(
        L"POST", url.c_str(), reqBody.data(), static_cast<uint32_t>(reqBody.size()),
        L"Content-Type: application/json\r\n",
        5000, 5000, 10000, 20000, 4u * 1024 * 1024);

    if (!r.ok || r.status != 200 || !JsonTrue(r.body, "success")) {
        std::string reason;
        if (!JsonString(r.body, "reason", reason) && !JsonString(r.body, "error", reason))
            reason = "Server error " + std::to_string(r.status);
        LOG_WARN("TokeerBridge: redeem failed (HTTP {}): {}", r.status, reason);
        Warn("SteamFlipper", "Redeem failed:\n\n" + reason);
        return;
    }

    std::string appIdStr, appHex, etHex;
    JsonString(r.body, "app_id", appIdStr);
    JsonString(r.body, "appticket", appHex);
    JsonString(r.body, "eticket", etHex);

    const auto appId = SFPlatform::Numbers::ParseUInt32(appIdStr);
    const auto appBytes = HexToBytes(appHex);
    const auto etBytes = HexToBytes(etHex);
    if (!appId || !appBytes || appBytes->empty() || !etBytes || etBytes->empty()) {
        LOG_WARN("TokeerBridge: redeem returned an incomplete/invalid ticket");
        Warn("SteamFlipper", "The server returned an incomplete ticket.");
        return;
    }

    if (CS::WriteAppTicket(*appId, *appBytes) != CS::Status::Ok ||
        CS::WriteETicket(*appId, *etBytes) != CS::Status::Ok) {
        LOG_ERROR("TokeerBridge: failed to write tickets for app {}", *appId);
        Warn("SteamFlipper", "Could not write the ticket to Steam.");
        return;
    }

    LOG_INFO("TokeerBridge: redeemed code for app {} ({} + {} bytes)",
             *appId, appBytes->size(), etBytes->size());

    const bool launchNow = SFPlatform::Dialog::ShowConfirm(
        "Code Redeemed!",
        "Added app " + std::to_string(*appId) + " to your library.\n\nLaunch it now?");
    if (launchNow) {
        const std::string steamUrl = "steam://rungameid/" + std::to_string(*appId);
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", steamUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        SFPlatform::Process::LaunchDetachedHidden("xdg-open '" + steamUrl + "'");
#endif
        LOG_INFO("TokeerBridge: launching app {} via {}", *appId, steamUrl);
    }
}

void HandleUri(const std::string& rawUrl) {
    LOG_INFO("TokeerBridge: HandleUri raw='{}'", rawUrl);

    std::string url = rawUrl;
    size_t begin = url.find_first_not_of(" \t\r\n");
    size_t end = url.find_last_not_of(" \t\r\n");
    url = (begin == std::string::npos) ? "" : url.substr(begin, end - begin + 1);
    if (url.size() >= 2 &&
        ((url.front() == '"' && url.back() == '"') ||
         (url.front() == '\'' && url.back() == '\'')))
        url = url.substr(1, url.size() - 2);

    const std::string prefix = std::string(kUriScheme) + "://";
    if (url.rfind(prefix, 0) != 0) {
        LOG_WARN("TokeerBridge: ignoring non-{} URL (cleaned='{}')", kUriScheme, url);
        return;
    }
    std::string rest = url.substr(prefix.size());
    while (!rest.empty() && (rest.back() == '/' || rest.back() == '\r' ||
                             rest.back() == '\n' || rest.back() == ' '))
        rest.pop_back();

    const size_t slash = rest.find('/');
    const std::string action = rest.substr(0, slash);
    const std::string arg = (slash == std::string::npos) ? "" : rest.substr(slash + 1);

    if (action == "redeem") {
        if (!arg.empty()) Redeem(arg);
        else Warn("SteamFlipper", "Missing code in link.");
    } else {
        LOG_WARN("TokeerBridge: unknown action '{}'", action);
    }
}

void RegisterUriScheme(const std::string& dllPath) {
#ifdef _WIN32
    const std::string command =
        "rundll32.exe \"" + dllPath + "\",TokeerUri \"%1\"";

    auto writeKey = [](const char* sub, const char* valueName, const std::string& value) -> bool {
        HKEY key{};
        if (RegCreateKeyExA(HKEY_CURRENT_USER, sub, 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        const LSTATUS s = RegSetValueExA(key, valueName, 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(value.c_str()),
                                         static_cast<DWORD>(value.size() + 1));
        RegCloseKey(key);
        return s == ERROR_SUCCESS;
    };

    const bool ok =
        writeKey("Software\\Classes\\bst", nullptr, "URL:SteamFlipper") &&
        writeKey("Software\\Classes\\bst", "URL Protocol", "") &&
        writeKey("Software\\Classes\\bst\\shell\\open\\command", nullptr, command);

    if (ok) LOG_INFO("TokeerBridge: registered bst:// scheme -> {}", command);
    else    LOG_WARN("TokeerBridge: failed to register bst:// scheme");
#else
    (void)dllPath;
#endif
}

} // namespace TokeerBridge
