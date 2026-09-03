#include "LuaFlipperPages.h"

#include "SteamFlipperBuildInfo.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/SteamMetadata/ManifestClient.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace LuaFlipperPages {
namespace {

    namespace fs = std::filesystem;

    /* ------------------------------------------------------------ helpers --- */

    // Escape into a JSON string body (no surrounding quotes).
    std::string JsonEscape(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 16);
        for (unsigned char c : in) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    std::string ReadWholeFile(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // One {"label","value"} object of a "rows" document.
    std::string Row(const char* label, const std::string& value, bool first) {
        return std::string(first ? "" : ",") +
               "{\"label\":\"" + label + "\",\"value\":\"" + JsonEscape(value) + "\"}";
    }

    // Manifests run from a few hundred bytes to tens of megabytes, so raw byte
    // counts would be unreadable in the panel.
    std::string Bytes(uintmax_t n) {
        if (n < 1024u)         return std::to_string(n) + " B";
        if (n < 1024u * 1024u) return std::to_string(n / 1024u) + " KiB";
        return std::to_string(n / (1024u * 1024u)) + " MiB";
    }

    /* -------------------------------------------------------------- fixes --- */

    struct AppIdCounts {
        size_t keyless = 0;
        size_t keyed   = 0;
    };

    // addappid(id) grants ownership only; addappid(id, n, "<64 hex>") also carries
    // the depot decryption key. Scanned per line so the commented-out entries the
    // manifest packs ship (a "--" prefix) are not counted as live, which the
    // whole-body scan on the manifests page cannot distinguish.
    AppIdCounts CountAppIds(const std::string& body) {
        AppIdCounts counts;
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            const size_t start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos) continue;
            if (line.compare(start, 2, "--") == 0) continue;
            for (size_t i = line.find("addappid(", start); i != std::string::npos;
                 i = line.find("addappid(", i + 1)) {
                const size_t end = line.find(')', i);
                if (end == std::string::npos) break;
                if (line.find('"', i) < end) counts.keyed++;
                else                          counts.keyless++;
            }
        }
        return counts;
    }

    std::vector<fs::path> FilesWithExtension(const fs::path& dir, const char* ext) {
        std::vector<fs::path> out;
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, ec);
             !ec && it != fs::directory_iterator(); ++it) {
            if (it->is_regular_file(ec) && it->path().extension() == ext)
                out.push_back(it->path());
        }
        return out;
    }

    std::string JsonFixes(const std::string& steamPath) {
        std::string j = "{\"fixes\":[";
        bool first = true;
        auto entry = [&](const std::string& file, const char* kind,
                         const std::string& detail) {
            j += first ? "" : ",";
            first = false;
            j += "{\"file\":\"" + JsonEscape(file) + "\"";
            j += ",\"kind\":\"" + JsonEscape(kind) + "\"";
            j += ",\"detail\":\"" + JsonEscape(detail) + "\"}";
        };

        // The same directory set the loader watches, so the page can never list a
        // file Steam was not actually told about.
        std::vector<fs::path> luas;
        for (const std::string& dir : LuaConfig::MergeWatchDirs(
                 Config::GetLuaPaths(),
                 (fs::path(steamPath) / "config" / "stplug-in").string())) {
            const std::vector<fs::path> found = FilesWithExtension(dir, ".lua");
            luas.insert(luas.end(), found.begin(), found.end());
        }
        std::sort(luas.begin(), luas.end());

        for (const auto& p : luas) {
            const AppIdCounts counts = CountAppIds(ReadWholeFile(p));
            if (counts.keyless == 0) continue;   // every id is keyed: not a fix
            entry(p.filename().string(),
                  counts.keyed ? "ownership" : "ownership only",
                  std::to_string(counts.keyless) + " keyless appid(s), " +
                      (counts.keyed ? std::to_string(counts.keyed) + " keyed"
                                    : "no decryption keys"));
        }

        // Manifests are the other half of a drop-in fix: without one the depot has
        // no file list and the download never starts, so a Lua entry alone is not
        // enough. Listing them makes the missing half visible.
        std::vector<fs::path> manifests =
            FilesWithExtension(fs::path(steamPath) / "depotcache", ".manifest");
        std::sort(manifests.begin(), manifests.end());

        for (const auto& p : manifests) {
            const std::string stem = p.stem().string();   // <depotid>_<gid>
            const size_t sep = stem.find('_');
            std::error_code ec;
            const uintmax_t size = fs::file_size(p, ec);
            std::string detail = (sep == std::string::npos)
                                     ? stem
                                     : "depot " + stem.substr(0, sep) +
                                           ", gid " + stem.substr(sep + 1);
            detail += ", " + (ec ? std::string("unknown size") : Bytes(size));
            entry(p.filename().string(), "manifest", detail);
        }

        j += "]}";
        return j;
    }

    /* -------------------------------------------------------------- cloud --- */

    std::string JsonCloud(const std::string& steamPath) {
        const Config::CloudSettings cloud = Config::GetCloudSettings();

        // Mirrors CloudRedirectHost::ResolveLibraryPath so the row shows the file
        // that would actually be loaded, not the raw config string.
        fs::path lib(cloud.library);
        if (cloud.library.empty()) {
#ifdef _WIN32
            lib = fs::path(steamPath) / "cloud_redirect.dll";
#else
            lib = fs::path(steamPath) / "cloud_redirect.so";
#endif
        } else if (lib.is_relative()) {
            lib = fs::path(steamPath) / lib;
        }

        std::error_code ec;
        const bool present = fs::exists(lib, ec);

        std::string j = "{\"rows\":[";
        j += Row("Enabled", cloud.enabled ? "yes" : "no", true);
        j += Row("Library", lib.string() + (present ? "" : " (not found)"), false);
        // Enabled but inactive means the library failed to load or CR_InitCloudSave
        // refused, which is the distinction worth surfacing here.
        j += Row("Redirecting saves", CloudRedirectHost::IsActive() ? "yes" : "no", false);
        j += "]}";
        return j;
    }

    /* ------------------------------------------------------------- config --- */

    std::string JsonConfig(const std::string& steamPath) {
        // dllmain builds the same path off the Steam root; recomputed from
        // steamPath so this component does not have to pull in dllmain.h.
        const fs::path toml = fs::path(steamPath) / "steamflipper.toml";
        std::error_code ec;

        std::string luaPaths;
        for (const std::string& p : Config::GetLuaPaths()) {
            if (!luaPaths.empty()) luaPaths += ", ";
            luaPaths += p;
        }
        if (luaPaths.empty())
            luaPaths = (fs::path(steamPath) / "config" / "stplug-in").string() +
                       " (built-in default)";

        std::string j = "{\"rows\":[";
        j += Row("Config file", toml.string(), true);
        j += Row("Config present",
                 fs::exists(toml, ec) ? "yes" : "no, built-in defaults in use", false);
        j += Row("Log directory", Config::GetLogDir(), false);
        j += Row("Lua paths", luaPaths, false);
        // Provider selection is table-driven inside ManifestClient, not stored on
        // Config, so the live selection is the only honest source here.
        j += Row("Manifest provider", ManifestClient::ActiveProviderName(), false);
        j += Row("Diagnostics popups", Config::GetDiagnosticsPopups() ? "on" : "off", false);
        j += Row("Update check", Config::GetUpdateEnabled() ? "enabled" : "disabled", false);
        const std::string repo = Config::GetUpdateRepo();
        j += Row("Source tree", repo.empty()
                     ? "not set ([update].repo, needed to pull updates)" : repo, false);
        j += Row("Client UI", Config::GetUiEnabled() ? "enabled" : "disabled", false);
        j += "]";

        // Alongside the rows rather than among them: the update panel on this
        // page draws these three, and a build the page can identify without a
        // network call is what lets it open showing something true.
        j += ",\"version\":\"" + JsonEscape(STEAMFLIPPER_VERSION) + "\"";
        j += ",\"sha\":\"" + JsonEscape(STEAMFLIPPER_GIT_SHA) + "\"";
        j += ",\"branch\":\"" + JsonEscape(STEAMFLIPPER_GIT_BRANCH) + "\"";
        j += "}";
        return j;
    }

} // namespace

std::string Render(const std::string& path, const std::string& steamPath) {
    if (path == "/api/fixes")  return JsonFixes(steamPath);
    if (path == "/api/cloud")  return JsonCloud(steamPath);
    if (path == "/api/config") return JsonConfig(steamPath);
    return {};
}

} // namespace LuaFlipperPages
