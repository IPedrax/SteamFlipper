#include "LuaFlipperUI.h"

#include "LuaFlipperDownload.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/CloudSaves/CloudSaves.h"
#include "LuaFlipperPages.h"
#include "SFPlatform/include/Hash.h"
#include "SFPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/Update/AppUpdater.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace LuaFlipperUI {
namespace {

#if defined(__linux__)

    // Steam's CEF debugger. Fixed at 8080 by the client; the marker file
    // <Steam>/.cef-enable-remote-debugging is what turns it on.
    constexpr uint16_t kCdpPort  = 8080;
    // Where the UI fetches its data from. Loopback only.
    constexpr uint16_t kApiPort  = 1987;

    std::string g_steamPath;
    std::atomic<bool> g_injected{false};

    /* ------------------------------------------------------------ helpers --- */

    std::string ReadWholeFile(const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

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

    // Value of a JSON string field, scanning forward from `from`. Deliberately
    // minimal: the only documents parsed here are CDP's own /json/list and
    // single-field replies, both machine-generated and flat.
    std::string JsonString(const std::string& doc, const std::string& key, size_t from = 0) {
        const std::string needle = "\"" + key + "\"";
        size_t k = doc.find(needle, from);
        if (k == std::string::npos) return {};
        size_t c = doc.find(':', k + needle.size());
        if (c == std::string::npos) return {};
        size_t q = doc.find('"', c);
        if (q == std::string::npos) return {};
        std::string out;
        for (size_t i = q + 1; i < doc.size(); i++) {
            if (doc[i] == '\\' && i + 1 < doc.size()) {
                char n = doc[++i];
                out += (n == 'n') ? '\n' : (n == 't') ? '\t' : (n == 'r') ? '\r' : n;
                continue;
            }
            if (doc[i] == '"') break;
            out += doc[i];
        }
        return out;
    }

    int ConnectLoopback(uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(fd);
            return -1;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // Both directions must time out. Reads here run until the peer closes,
        // and CEF does not always close when asked; without this the injector
        // thread blocks forever inside recv() on its first request, producing no
        // log line at all because the loop never comes back around.
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        return fd;
    }

    bool SendAll(int fd, const char* p, size_t n) {
        while (n) {
            ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
            if (w <= 0) return false;
            p += w;
            n -= static_cast<size_t>(w);
        }
        return true;
    }

    /* --------------------------------------------------------------- CDP --- */

    // GET over a fresh loopback connection. Returns the body.
    std::string HttpGet(uint16_t port, const std::string& path) {
        int fd = ConnectLoopback(port);
        if (fd < 0) return {};
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
        std::string resp;
        if (SendAll(fd, req.data(), req.size())) {
            char buf[8192];
            for (;;) {
                ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) break;               // closed, or the recv timeout
                resp.append(buf, static_cast<size_t>(r));
                if (resp.size() > 4u * 1024 * 1024) break;

                // Stop as soon as the declared body has arrived, rather than
                // waiting for a close that may only come via the timeout.
                size_t hdrEnd = resp.find("\r\n\r\n");
                if (hdrEnd == std::string::npos) continue;
                size_t cl = resp.find("Content-Length:");
                if (cl == std::string::npos || cl > hdrEnd) continue;
                size_t want = strtoul(resp.c_str() + cl + 15, nullptr, 10);
                if (resp.size() - (hdrEnd + 4) >= want) break;
            }
        }
        ::close(fd);
        size_t sep = resp.find("\r\n\r\n");
        return sep == std::string::npos ? std::string() : resp.substr(sep + 4);
    }

    // Minimal RFC6455 client. Only what CDP needs: a handshake, masked text
    // frames out, frames in. curl is not used here on purpose - it is dlopen'd
    // from the steam-runtime, which pins an older libcurl that predates the
    // curl_ws_* API, so it cannot be relied on for this.
    struct WebSocket {
        int fd = -1;
        std::string rest;
        int nextId = 0;

        bool Open(const std::string& url) {
            // ws://127.0.0.1:8080/devtools/page/<id>
            size_t slash = url.find('/', url.find("//") + 2);
            if (slash == std::string::npos) return false;
            std::string path = url.substr(slash);

            fd = ConnectLoopback(kCdpPort);
            if (fd < 0) return false;

            std::string req =
                "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                // A fixed key is legitimate for a client: the server hashes it
                // into Sec-WebSocket-Accept, which we do not verify. It is not a
                // secret and carries no security role here.
                "Sec-WebSocket-Key: c3RlYW1mbGlwcGVyMTIzNA==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n";
            if (!SendAll(fd, req.data(), req.size())) { Close(); return false; }

            std::string hdr;
            char buf[2048];
            while (hdr.find("\r\n\r\n") == std::string::npos) {
                ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) { Close(); return false; }
                hdr.append(buf, static_cast<size_t>(r));
                if (hdr.size() > 65536) { Close(); return false; }
            }
            if (hdr.find(" 101") == std::string::npos) { Close(); return false; }
            rest = hdr.substr(hdr.find("\r\n\r\n") + 4);
            return true;
        }

        bool Send(const std::string& payload) {
            std::string frame;
            frame += static_cast<char>(0x81);              // FIN + text
            const size_t n = payload.size();
            if (n < 126) {
                frame += static_cast<char>(0x80 | n);
            } else if (n < 65536) {
                frame += static_cast<char>(0x80 | 126);
                frame += static_cast<char>((n >> 8) & 0xFF);
                frame += static_cast<char>(n & 0xFF);
            } else {
                // Widen before shifting. This module is i386, so size_t is 32
                // bits and `n >> 56` is undefined; x86 masks the shift count to
                // 5 bits, which silently yields `n >> 24` and a garbage length.
                // The bootstrap only crossed 64 KiB once the UI grew, so this
                // path stayed dormant and then broke injection outright: the
                // send still succeeded, CEF discarded the malformed frame, and
                // nothing executed.
                const uint64_t w = static_cast<uint64_t>(n);
                frame += static_cast<char>(0x80 | 127);
                for (int i = 7; i >= 0; i--)
                    frame += static_cast<char>((w >> (i * 8)) & 0xFF);
            }
            // Client frames must be masked. The mask is anti-proxy-cache
            // machinery, not security, so a fixed one is fine.
            const unsigned char mask[4] = { 0x21, 0x5A, 0x7C, 0x03 };
            frame.append(reinterpret_cast<const char*>(mask), 4);
            for (size_t i = 0; i < n; i++)
                frame += static_cast<char>(payload[i] ^ mask[i % 4]);
            return SendAll(fd, frame.data(), frame.size());
        }

        bool Call(const std::string& method, const std::string& paramsJson) {
            std::string msg = "{\"id\":" + std::to_string(++nextId) +
                              ",\"method\":\"" + method + "\"";
            if (!paramsJson.empty()) msg += ",\"params\":" + paramsJson;
            msg += "}";
            if (!Send(msg)) return false;
            // Drain one frame so replies do not pile up in the socket buffer.
            Recv();
            return true;
        }

        std::string Recv() {
            auto need = [&](size_t n) -> bool {
                char buf[8192];
                while (rest.size() < n) {
                    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
                    if (r <= 0) return false;
                    rest.append(buf, static_cast<size_t>(r));
                }
                return true;
            };
            if (!need(2)) return {};
            unsigned char b1 = static_cast<unsigned char>(rest[1]);
            size_t len = b1 & 0x7F;
            size_t off = 2;
            if (len == 126) {
                if (!need(4)) return {};
                len = (static_cast<unsigned char>(rest[2]) << 8) |
                       static_cast<unsigned char>(rest[3]);
                off = 4;
            } else if (len == 127) {
                if (!need(10)) return {};
                len = 0;
                for (int i = 2; i < 10; i++)
                    len = (len << 8) | static_cast<unsigned char>(rest[i]);
                off = 10;
            }
            if (!need(off + len)) return {};
            std::string out = rest.substr(off, len);
            rest.erase(0, off + len);
            return out;
        }

        void Close() {
            if (fd >= 0) { ::close(fd); fd = -1; }
        }
    };

    // Steam's menu styling sits on class names that are hashed per client build;
    // the readable aliases beside them carry no rules at all. Read the real ones
    // off a live Steam menu so the dropdown can wear them and inherit both the
    // stock look and any custom theme.
    std::string ReadSteamMenuClasses(const std::string& listJson) {
        size_t at = listJson.find("\"Account Menu\"");
        if (at == std::string::npos) return {};
        // webSocketDebuggerUrl of that entry; fields follow the title.
        std::string ws = JsonString(listJson, "webSocketDebuggerUrl", at);
        if (ws.empty()) return {};

        WebSocket s;
        if (!s.Open(ws)) return {};
        const char* expr =
            "(function(){var m=document.querySelector('.contextMenu'),"
            "c=document.querySelector('.contextMenuContents'),"
            "i=document.querySelector('.contextMenuItem');"
            "if(!m||!i)return '';"
            "return JSON.stringify({menu:m.className,contents:c?c.className:'',"
            "item:i.className});})()";
        s.Send("{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{"
               "\"expression\":\"" + JsonEscape(expr) + "\",\"returnByValue\":true}}");
        std::string reply = s.Recv();
        s.Close();

        // The value is itself a JSON document, delivered as a string.
        std::string v = JsonString(reply, "value");
        return v.find("menu") == std::string::npos ? std::string() : v;
    }

    bool InjectInto(const std::string& wsUrl, const std::string& bootstrap) {
        WebSocket s;
        if (!s.Open(wsUrl)) return false;

        const std::string src = JsonEscape(bootstrap);
        // Survive navigation inside this target...
        s.Call("Page.enable", "{}");
        s.Call("Page.addScriptToEvaluateOnNewDocument",
               "{\"source\":\"" + src + "\"}");
        // ...and take effect on the document already showing. Page.reload is
        // deliberately never used: reloading Steam's main window drops its React
        // app and leaves the client with a blank window.
        s.Call("Runtime.evaluate",
               "{\"expression\":\"" + src + "\",\"returnByValue\":false}");
        s.Close();
        return true;
    }

    /* ------------------------------------------------------------- pages --- */

    std::vector<std::filesystem::path> LuaFiles() {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        for (const std::string& dir : LuaConfig::MergeWatchDirs(
                 Config::GetLuaPaths(), (std::filesystem::path(g_steamPath) /
                                         "config" / "stplug-in").string())) {
            for (auto it = std::filesystem::directory_iterator(dir, ec);
                 !ec && it != std::filesystem::directory_iterator(); ++it) {
                if (it->is_regular_file(ec) && it->path().extension() == ".lua")
                    out.push_back(it->path());
            }
        }
        return out;
    }

    std::string JsonManifests() {
        std::string j = "{\"manifests\":[";
        bool first = true;
        for (const auto& p : LuaFiles()) {
            const std::string body = ReadWholeFile(p);
            // Count the two forms the loader understands, so the page can show
            // at a glance whether a manifest carries decryption keys or only
            // ownership. A file with no keyed line cannot decrypt anything.
            size_t keys = 0, ids = 0;
            for (size_t i = body.find("addappid("); i != std::string::npos;
                 i = body.find("addappid(", i + 1)) {
                // A keyed entry is addappid(id, 1, "<64 hex>").
                size_t end = body.find(')', i);
                if (end == std::string::npos) break;
                ids++;
                if (body.find('"', i) < end) keys++;
            }
            j += first ? "" : ",";
            first = false;
            j += "{\"file\":\"" + JsonEscape(p.filename().string()) + "\"";
            j += ",\"appid\":\"" + JsonEscape(p.stem().string()) + "\"";
            j += ",\"ids\":" + std::to_string(ids);
            j += ",\"keys\":" + std::to_string(keys) + "}";
        }
        j += "]}";
        return j;
    }

    std::string JsonStatus() {
        const std::vector<AppId_t> depots = LuaConfig::GetAllDepotIds();
        size_t owned = 0;
        for (AppId_t id : depots) if (LuaConfig::IsOwned(id)) owned++;

        std::string j = "{\"rows\":[";
        auto row = [&](const char* k, const std::string& v, bool first) {
            return std::string(first ? "" : ",") +
                   "{\"label\":\"" + k + "\",\"value\":\"" + JsonEscape(v) + "\"}";
        };
        j += row("Steam directory", g_steamPath, true);
        j += row("Lua manifests", std::to_string(LuaFiles().size()), false);
        j += row("Depots registered", std::to_string(depots.size()), false);
        j += row("Marked owned", std::to_string(owned), false);
        j += row("UI injected", g_injected ? "yes" : "not yet", false);
        j += row("API", "127.0.0.1:" + std::to_string(kApiPort), false);
        j += "]}";
        return j;
    }

    // The add page. Steam's store search is public, but the manifest sources
    // behind it are not: they sit behind a lua.tools account token with a daily
    // cap, which is the user's to supply and not something this module holds.
    // So report the local state truthfully instead of offering a download path
    // that could only ever fail.
    std::string JsonUnlocker() {
        std::string j = "{\"installed\":" + std::to_string(LuaFiles().size());
        j += ",\"results\":[],\"notes\":[";
        j += "{\"label\":\"Add by hand\",\"value\":\"Drop a .lua into " +
             JsonEscape((std::filesystem::path(g_steamPath) / "config" /
                         "stplug-in").string()) + "\"}";
        j += ",{\"label\":\"Ownership\",\"value\":\"Picked up live, no restart\"}";
        j += ",{\"label\":\"Depot keys\",\"value\":\"Need tools/sync_depot_keys.py "
             "with Steam closed\"}";
        j += "]}";
        return j;
    }

    // Value of a query parameter, or empty. Only used for appid/source, both of
    // which the download layer validates again before they reach a URL or path.
    std::string QueryParam(const std::string& path, const std::string& key) {
        size_t q = path.find('?');
        if (q == std::string::npos) return {};
        const std::string needle = key + "=";
        for (size_t at = q + 1; at < path.size();) {
            size_t end = path.find('&', at);
            if (end == std::string::npos) end = path.size();
            if (path.compare(at, needle.size(), needle) == 0) {
                // The page sends these through encodeURIComponent, so undo that
                // here; a search term reaches us as %20-separated words.
                const std::string raw =
                    path.substr(at + needle.size(), end - at - needle.size());
                std::string out;
                for (size_t i = 0; i < raw.size(); i++) {
                    if (raw[i] == '+') { out += ' '; continue; }
                    if (raw[i] == '%' && i + 2 < raw.size()) {
                        out += static_cast<char>(
                            strtoul(raw.substr(i + 1, 2).c_str(), nullptr, 16));
                        i += 2;
                        continue;
                    }
                    out += raw[i];
                }
                return out;
            }
            at = end + 1;
        }
        return {};
    }

    // Percent-encode a query value. The search term is user text and goes
    // straight into a URL.
    std::string UrlEncode(const std::string& in) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : in) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    /**
     * Proxy Steam's public store search.
     *
     * The page cannot call store.steampowered.com itself: it runs on
     * steamloopback.host and Steam's store sends no CORS headers, so the browser
     * would block the reply. Going through this server sidesteps that, and it is
     * the same public endpoint LuaTools uses, needing no account.
     */
    std::string JsonSearch(const std::string& term) {
        const std::string url =
            "https://store.steampowered.com/api/storesearch/?term=" +
            UrlEncode(term) + "&l=english&cc=US";
        auto r = SFPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, nullptr,
                                           5000, 5000, 10000, 15000, 512u * 1024u);
        if (!r.ok || r.status != 200)
            return "{\"error\":\"Steam store search is unreachable\"}";

        // Each item is {"type":..,"name":"..","id":N,..}; pair every name with the
        // id that follows it rather than parsing the whole document.
        std::string j = "{\"results\":[";
        bool first = true;
        for (size_t at = r.body.find("\"name\":"); at != std::string::npos;
             at = r.body.find("\"name\":", at + 1)) {
            const std::string name = JsonString(r.body, "name", at);
            size_t idAt = r.body.find("\"id\":", at);
            if (idAt == std::string::npos) break;
            const std::string id =
                std::to_string(strtoul(r.body.c_str() + idAt + 5, nullptr, 10));
            if (id == "0" || name.empty()) continue;
            j += first ? "" : ",";
            first = false;
            j += "{\"appid\":\"" + id + "\",\"name\":\"" + JsonEscape(name) + "\"}";
        }
        j += "]}";
        return j;
    }

    /**
     * Steam's featured lists, for the browse view's recommended rows.
     *
     * Public and unauthenticated, same as the store search, and proxied for the
     * same reason: the store sends no CORS headers to steamloopback.host.
     * Returns the first `limit` entries of one category as {appid, name}.
     */
    std::string JsonFeatured(const std::string& category, size_t limit) {
        auto r = SFPlatform::Http::Execute(
            L"GET", "https://store.steampowered.com/api/featuredcategories/?cc=US&l=english",
            nullptr, 0, nullptr, 5000, 5000, 10000, 20000, 2u * 1024u * 1024u);
        if (!r.ok || r.status != 200)
            return "{\"error\":\"Steam featured lists are unreachable\"}";

        // Scope the scan to the requested category; ids and names repeat across
        // every list in the document, so a global scan would mix them.
        size_t at = r.body.find("\"" + category + "\"");
        if (at == std::string::npos) return "{\"results\":[]}";
        size_t end = r.body.find("\"tabs\"", at);
        if (end == std::string::npos) end = r.body.size();

        std::string j = "{\"results\":[";
        size_t n = 0;
        for (size_t i = r.body.find("\"id\":", at);
             i != std::string::npos && i < end && n < limit;
             i = r.body.find("\"id\":", i + 1)) {
            const std::string id =
                std::to_string(strtoul(r.body.c_str() + i + 5, nullptr, 10));
            const std::string name = JsonString(r.body, "name", i);
            if (id == "0" || name.empty()) continue;
            j += n ? "," : "";
            j += "{\"appid\":\"" + id + "\",\"name\":\"" + JsonEscape(name) + "\"}";
            n++;
        }
        j += "]}";
        return j;
    }

    // The [...] or {...} beginning at `key`, brackets balanced, quotes honoured.
    // Sub-arrays of appdetails are already valid JSON, so forwarding them whole
    // beats reparsing them field by field.
    std::string JsonRegion(const std::string& doc, const std::string& key) {
        size_t k = doc.find("\"" + key + "\"");
        if (k == std::string::npos) return {};
        size_t s = doc.find_first_of("[{", k);
        if (s == std::string::npos) return {};
        const char open = doc[s], close = (open == '[') ? ']' : '}';
        int depth = 0;
        bool inStr = false;
        for (size_t i = s; i < doc.size(); i++) {
            const char c = doc[i];
            if (inStr) {
                if (c == '\\') { i++; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == open) depth++;
            else if (c == close && --depth == 0) return doc.substr(s, i - s + 1);
        }
        return {};
    }

    /**
     * Steam's own app details, proxied for the same CORS reason as the search.
     *
     * Only the fields the app page draws are forwarded, so the reply stays small
     * next to the original (which carries every screenshot, movie and DLC id).
     * header_image matters most: newer apps 404 on the flat cdn header.jpg path
     * because Steam moved that art to hashed store_item_assets URLs, and this is
     * where the working URL comes from.
     */
    std::string JsonAppDetails(const std::string& appId) {
        const std::string url =
            "https://store.steampowered.com/api/appdetails?appids=" + appId +
            "&cc=US&l=english";
        auto r = SFPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, nullptr,
                                           5000, 5000, 10000, 20000, 4u * 1024u * 1024u);
        if (!r.ok || r.status != 200)
            return "{\"error\":\"Steam app details are unreachable\"}";
        if (r.body.find("\"success\":true") == std::string::npos)
            return "{\"error\":\"Steam has no store page for this app\"}";

        std::string j = "{";
        j += "\"name\":\"" + JsonEscape(JsonString(r.body, "name")) + "\"";
        j += ",\"description\":\"" +
             JsonEscape(JsonString(r.body, "short_description")) + "\"";
        j += ",\"header\":\"" + JsonEscape(JsonString(r.body, "header_image")) + "\"";

        size_t rd = r.body.find("\"release_date\"");
        j += ",\"released\":\"" +
             JsonEscape(rd == std::string::npos ? std::string()
                                                : JsonString(r.body, "date", rd)) + "\"";

        auto region = [&](const char* key, const char* out) {
            const std::string v = JsonRegion(r.body, key);
            j += std::string(",\"") + out + "\":" + (v.empty() ? "[]" : v);
        };
        region("developers", "developers");
        region("publishers", "publishers");
        region("genres", "genres");

        // Screenshots for the gallery. Capped because an app can ship 40+ and
        // the page only ever shows a strip of thumbnails; forwarding them all
        // would multiply the reply size for rows nobody scrolls to.
        j += ",\"screenshots\":[";
        size_t shots = 0;
        for (size_t i = r.body.find("\"path_thumbnail\"");
             i != std::string::npos && shots < 12;
             i = r.body.find("\"path_thumbnail\"", i + 1)) {
            const std::string thumb = JsonString(r.body, "path_thumbnail", i);
            const std::string full  = JsonString(r.body, "path_full", i);
            if (thumb.empty()) continue;
            j += shots ? "," : "";
            j += "{\"thumb\":\"" + JsonEscape(thumb) + "\",\"full\":\"" +
                 JsonEscape(full.empty() ? thumb : full) + "\"}";
            shots++;
        }
        j += "]}";
        return j;
    }

    /**
     * Authoritative art URLs for an app, from IStoreBrowseService/GetItems.
     *
     * This is the endpoint Steam's own store uses, it needs no API key, and it
     * is the only way to get the per-asset hashes. The flat
     * cdn.../steam/apps/<id>/header.jpg path 404s for newer apps because Steam
     * moved that art behind hashed store_item_assets URLs, so guessing paths
     * leaves tiles blank; here every filename arrives already hashed.
     *
     * Also the only source for two things nothing else exposes: community_icon,
     * the small square icon beside a store page title, and page_background, the
     * per-game backdrop that makes an app page look like Steam rather than like
     * a dark div.
     */
    std::string JsonAssets(const std::string& appId) {
        const std::string q =
            "{\"ids\":[{\"appid\":" + appId + "}],\"context\":{\"language\":"
            "\"english\",\"country_code\":\"US\"},\"data_request\":"
            "{\"include_assets\":true,\"include_release\":true,"
            "\"include_basic_info\":true}}";
        const std::string url =
            "https://api.steampowered.com/IStoreBrowseService/GetItems/v1/"
            "?input_json=" + UrlEncode(q);
        auto r = SFPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, nullptr,
                                           5000, 5000, 10000, 20000, 1024u * 1024u);
        if (!r.ok || r.status != 200)
            return "{\"error\":\"Steam store service is unreachable\"}";

        const std::string fmt = JsonString(r.body, "asset_url_format");
        if (fmt.empty()) return "{\"error\":\"no store item for this app\"}";

        // asset_url_format carries a ${FILENAME} placeholder; every other asset
        // field is the path that goes into it.
        const std::string base =
            "https://shared.cloudflare.steamstatic.com/store_item_assets/";
        const size_t ph = fmt.find("${FILENAME}");
        auto resolve = [&](const char* key) {
            const std::string v = JsonString(r.body, key);
            if (v.empty() || ph == std::string::npos) return std::string();
            return base + fmt.substr(0, ph) + v + fmt.substr(ph + 11);
        };

        std::string j = "{\"name\":\"" + JsonEscape(JsonString(r.body, "name")) + "\"";
        const char* keys[] = { "header", "library_hero", "page_background",
                               "main_capsule", "small_capsule", "hero_capsule" };
        for (const char* k : keys)
            j += ",\"" + std::string(k) + "\":\"" + JsonEscape(resolve(k)) + "\"";

        // The community icon is a bare hash on a different host, not an
        // asset_url_format path.
        const std::string icon = JsonString(r.body, "community_icon");
        j += ",\"icon\":\"" + JsonEscape(icon.empty() ? std::string() :
             "https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/"
             "apps/" + appId + "/" + icon + ".jpg") + "\"";
        j += "}";
        return j;
    }

    /**
     * Review summary, for the store page's RECENT/ENGLISH REVIEWS rows. Public,
     * no key. num_per_page=0 asks for the summary without any review bodies.
     */
    std::string JsonReviews(const std::string& appId) {
        const std::string url =
            "https://store.steampowered.com/appreviews/" + appId +
            "?json=1&num_per_page=0&language=all";
        auto r = SFPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, nullptr,
                                           5000, 5000, 10000, 15000, 256u * 1024u);
        if (!r.ok || r.status != 200)
            return "{\"error\":\"Steam reviews are unreachable\"}";

        auto number = [&](const char* key) {
            size_t k = r.body.find(std::string("\"") + key + "\"");
            if (k == std::string::npos) return std::string("0");
            size_t c = r.body.find(':', k);
            if (c == std::string::npos) return std::string("0");
            return std::to_string(strtoul(r.body.c_str() + c + 1, nullptr, 10));
        };
        std::string j = "{\"summary\":\"" +
            JsonEscape(JsonString(r.body, "review_score_desc")) + "\"";
        j += ",\"positive\":" + number("total_positive");
        j += ",\"negative\":" + number("total_negative");
        j += ",\"total\":" + number("total_reviews") + "}";
        return j;
    }

    /**
     * Take a manifest out of service.
     *
     * Renamed, not deleted. A manifest is the only copy of its depot decryption
     * keys, and re-downloading is not always possible: a source can stop
     * carrying an app, and hand-written files have no source at all. Renaming
     * costs a stale file in the folder and makes a misclick recoverable, which
     * an unlink does not.
     *
     * The loader only reads *.lua, so the renamed file is inert immediately and
     * the watcher drops its ownership entries on the next scan.
     */
    std::string JsonRemove(const std::string& appId) {
        std::error_code ec;
        for (const auto& p : LuaFiles()) {
            if (p.stem().string() != appId) continue;
            std::filesystem::path gone = p;
            gone += ".removed";
            std::filesystem::remove(gone, ec);           // a previous removal
            std::filesystem::rename(p, gone, ec);
            if (ec) {
                return "{\"error\":\"could not remove " +
                       JsonEscape(p.filename().string()) + ": " +
                       JsonEscape(ec.message()) + "\"}";
            }
            LOG_INFO("LuaFlipperUI: removed {} (kept as {})",
                     p.filename().string(), gone.filename().string());
            return "{\"ok\":true,\"kept\":\"" +
                   JsonEscape(gone.filename().string()) + "\"}";
        }
        return "{\"error\":\"no manifest for that app id\"}";
    }

    /**
     * Re-download a manifest over the installed one.
     *
     * Picks the first source that answers, same as the Unlocker's Add, because
     * an update is the same fetch aimed at an app already present. The existing
     * file is left alone until the download succeeds, so a failed update cannot
     * lose the manifest that was working.
     */
    std::string JsonUpdate(const std::string& appId) {
        const std::string probe = LuaFlipperDownload::ProbeSources(appId);
        std::string last = "{\"error\":\"no source carries this app\"}";

        for (size_t at = probe.find("\"name\""); at != std::string::npos;
             at = probe.find("\"name\"", at + 1)) {
            const std::string name = JsonString(probe, "name", at);
            const std::string status = JsonString(probe, "status", at);
            if (name.empty() || status == "unavailable") continue;

            LOG_INFO("LuaFlipperUI: updating {} from {}", appId, name);
            const std::string res = LuaFlipperDownload::Install(appId, name, g_steamPath);
            if (res.find("\"ok\":true") != std::string::npos) return res;
            last = res;
        }
        return last;
    }

    /**
     * CloudRedirect's state, as far as this process can actually observe it.
     *
     * CloudRedirect (github.com/Selectively11/CloudRedirect) gives Lua-unlocked
     * apps working Steam Cloud by redirecting the client's cloud RPCs to Google
     * Drive, OneDrive, R2, S3 or a local folder. SteamFlipper does not implement
     * any of that; CloudRedirectHost loads CloudRedirect's own library and
     * forwards Cloud.* RPCs to it.
     *
     * Everything here is a fact this process can check: whether the library is
     * on disk, whether it loaded, and how many unlocked apps it claimed. The
     * provider and the sync log live behind CloudRedirect's own D-Bus service
     * (org.cloudredirect.Service: GetStatus / GetManagedApps / GetConfig), which
     * this module does not speak, so those are reported as unknown rather than
     * guessed at.
     */
    std::string JsonCloud() {
        const Config::CloudSettings cloud = Config::GetCloudSettings();
        const bool active = CloudSaves::IsActive();

        size_t claimed = 0;
        if (active) {
            for (AppId_t id : LuaConfig::GetAllDepotIds())
                if (CloudSaves::IsApp(id)) claimed++;
        }

        const std::filesystem::path store =
            std::filesystem::path(g_steamPath) / "steamflipper" / "cloudsaves";

        // "installed" is retained for the page's status panel but is now always
        // true: the backend is compiled in, so there is nothing to install. It
        // used to report whether CloudRedirect's library was on disk.
        std::string j = "{\"enabled\":" + std::string(cloud.enabled ? "true" : "false");
        j += ",\"installed\":true";
        j += ",\"active\":" + std::string(active ? "true" : "false");
        j += ",\"builtin\":true";
        j += ",\"storage\":\"" + JsonEscape(store.string()) + "\"";
        j += ",\"apps\":" + std::to_string(claimed);
        j += ",\"config\":\"" +
             JsonEscape((std::filesystem::path(g_steamPath) / "steamflipper.toml").string()) +
             "\"}";
        return j;
    }

    /**
     * Turn on [cloud] in steamflipper.toml.
     *
     * Append-only. If a [cloud] section already exists this refuses and says so
     * rather than rewriting it: the file is hand-edited, may carry comments and
     * a library path the user set deliberately, and a config rewriter that
     * silently reformats someone's file is a bad trade for saving one edit.
     */
    /**
     * One switch: install the library if needed, point it at a local folder, and
     * turn [cloud] on.
     *
     * The local provider is the default because it is the only one that needs no
     * sign-in: LocalDiskProvider::Init just takes a path and creates it. Google
     * Drive, OneDrive, R2 and S3 all need OAuth or keys, and those flows live in
     * CloudRedirect's own app, so a toggle cannot stand in for them. Saves land
     * in a folder the user can point a sync client at if they want them off the
     * machine.
     */
    std::string JsonCloudEnable() {
        // Nothing to fetch or configure any more. The save backend is compiled
        // in (Utils/CloudSaves), so enabling is only a matter of flipping the
        // flag its Initialize() reads at startup. This used to download
        // CloudRedirect's library and write its config.json; both were dropped
        // when the backend moved in-process.
        const std::filesystem::path cfg =
            std::filesystem::path(g_steamPath) / "steamflipper.toml";
        const std::filesystem::path lib =
            std::filesystem::path(g_steamPath) / "steamflipper" / "cloudsaves";
        std::string body = ReadWholeFile(cfg);
        if (body.find("[cloud]") == std::string::npos) {
            std::ofstream f(cfg, std::ios::app);
            if (!f) return "{\"error\":\"could not write " + JsonEscape(cfg.string()) + "\"}";
            // No library key. It named CloudRedirect's .so back when the saves
            // went through it; the backend is compiled in now, so writing a path
            // to a file that need not exist only invites someone to fix it.
            f << "\n[cloud]\nenabled = true\n";
        } else {
            // Flip the existing flag rather than appending a second section.
            const size_t at = body.find("enabled", body.find("[cloud]"));
            const size_t f = (at == std::string::npos) ? std::string::npos
                                                       : body.find("false", at);
            if (f != std::string::npos && f < at + 32) body.replace(f, 5, "true");
            std::ofstream w(cfg, std::ios::trunc);
            if (!w) return "{\"error\":\"could not write " + JsonEscape(cfg.string()) + "\"}";
            w << body;
        }

        LOG_INFO("LuaFlipperUI: cloud saves enabled");
        return "{\"ok\":true,\"config\":\"" + JsonEscape(cfg.string()) +
               "\",\"storage\":\"" + JsonEscape(lib.string()) + "\"}";
    }

    /** Turn [cloud] off. The library and any saves already written are left. */
    std::string JsonCloudDisable() {
        const std::filesystem::path cfg =
            std::filesystem::path(g_steamPath) / "steamflipper.toml";
        std::string body = ReadWholeFile(cfg);
        const size_t sec = body.find("[cloud]");
        if (sec == std::string::npos)
            return "{\"ok\":true,\"note\":\"cloud was not enabled\"}";

        const size_t at = body.find("enabled", sec);
        const size_t t = (at == std::string::npos) ? std::string::npos
                                                   : body.find("true", at);
        if (t != std::string::npos && t < at + 32) body.replace(t, 4, "false");
        std::ofstream w(cfg, std::ios::trunc);
        if (!w) return "{\"error\":\"could not write " + JsonEscape(cfg.string()) + "\"}";
        w << body;

        LOG_INFO("LuaFlipperUI: cloud saves disabled");
        return "{\"ok\":true,\"config\":\"" + JsonEscape(cfg.string()) + "\"}";
    }

    /**
     * Is this install running older source than the branch it came from?
     *
     * Read-only and network-only: one unauthenticated GitHub call, nothing
     * touched on disk. There is no published .so to compare against, because
     * the module is built locally, so the comparison is between the commit
     * baked in at build time and the head of that branch.
     *
     * A build made outside a checkout has no sha to compare and says so
     * through `reason`; that is not an error and must not be drawn as one.
     */
    std::string JsonSourceCheck() {
        const AppUpdater::SourceCheck s = AppUpdater::CheckSource();
        if (!s.error.empty())
            return "{\"error\":\"" + JsonEscape(s.error) + "\"}";

        std::string j = "{\"sha\":\"" + JsonEscape(s.sha) + "\"";
        j += ",\"branch\":\"" + JsonEscape(s.branch) + "\"";
        if (!s.reason.empty()) {
            j += ",\"behind\":false,\"reason\":\"" + JsonEscape(s.reason) + "\"}";
            return j;
        }
        j += ",\"remote\":\"" + JsonEscape(s.remote) + "\"";
        j += ",\"message\":\"" + JsonEscape(s.message) + "\"";
        j += ",\"behind\":" + std::string(s.behind ? "true" : "false") + "}";
        return j;
    }

    /**
     * Fast-forward the source checkout named by [update].repo.
     *
     * This updates the source and nothing else. The module Steam has mapped is
     * built by tools/install_linux.sh, which needs the 32-bit toolchain and
     * refuses to run while Steam is up, so the rebuild stays the user's step
     * and the reply carries the exact command rather than implying the update
     * is already live.
     *
     * Nothing here is destructive: a dirty tree or a non-fast-forward stops
     * the operation instead of being stashed, reset or forced past.
     */
    std::string JsonSourceApply() {
        const AppUpdater::PullResult p = AppUpdater::PullSource();
        if (!p.ok) {
            return "{\"error\":\"" + JsonEscape(p.error) + "\",\"status\":\"" +
                   JsonEscape(p.status) + "\"}";
        }
        std::string j = "{\"ok\":true";
        j += ",\"pulled\":\"" + JsonEscape(p.sha) + "\"";
        j += ",\"status\":\"" + JsonEscape(p.status) + "\"";
        j += ",\"note\":\"run ./tools/install_linux.sh with Steam closed\"";
        j += ",\"command\":\"" +
             JsonEscape("cd " + p.repo + " && ./tools/install_linux.sh") + "\"}";
        return j;
    }

    std::string RouteApi(const std::string& fullPath) {
        const std::string path = fullPath.substr(0, fullPath.find('?'));

        if (path == "/api/manifests") return JsonManifests();
        if (path == "/api/status")    return JsonStatus();

        // Finding and installing a pack. Both take ?appid=, install also
        // ?source=; these are the only endpoints that reach the network.
        if (path == "/api/search") {
            const std::string term = QueryParam(fullPath, "term");
            if (term.empty()) return "{\"results\":[]}";
            return JsonSearch(term);
        }
        if (path == "/api/cloud")        return JsonCloud();
        if (path == "/api/cloud/enable")  return JsonCloudEnable();
        if (path == "/api/cloud/disable") return JsonCloudDisable();
        if (path == "/api/remove") {
            const std::string appId = QueryParam(fullPath, "appid");
            if (appId.empty()) return "{\"error\":\"no appid given\"}";
            return JsonRemove(appId);
        }
        // /api/update is a manifest re-download for one app; the two below are
        // SteamFlipper updating itself. Same word, unrelated jobs, and the
        // match is exact so the shared prefix costs nothing.
        if (path == "/api/update") {
            const std::string appId = QueryParam(fullPath, "appid");
            if (appId.empty()) return "{\"error\":\"no appid given\"}";
            return JsonUpdate(appId);
        }
        if (path == "/api/update/check") return JsonSourceCheck();
        if (path == "/api/update/apply") return JsonSourceApply();
        if (path == "/api/assets") {
            const std::string appId = QueryParam(fullPath, "appid");
            if (appId.empty()) return "{\"error\":\"no appid given\"}";
            return JsonAssets(appId);
        }
        if (path == "/api/reviews") {
            const std::string appId = QueryParam(fullPath, "appid");
            if (appId.empty()) return "{\"error\":\"no appid given\"}";
            return JsonReviews(appId);
        }
        if (path == "/api/appdetails") {
            const std::string appId = QueryParam(fullPath, "appid");
            if (appId.empty()) return "{\"error\":\"no appid given\"}";
            return JsonAppDetails(appId);
        }
        if (path == "/api/featured") {
            std::string cat = QueryParam(fullPath, "list");
            if (cat != "top_sellers" && cat != "new_releases" && cat != "specials")
                cat = "top_sellers";
            return JsonFeatured(cat, 12);
        }
        if (path == "/api/sources") {
            const std::string appId = QueryParam(fullPath, "appid");
            if (appId.empty()) return "{\"error\":\"no appid given\"}";
            return LuaFlipperDownload::ProbeSources(appId);
        }
        if (path == "/api/install") {
            const std::string appId  = QueryParam(fullPath, "appid");
            const std::string source = QueryParam(fullPath, "source");
            if (appId.empty() || source.empty())
                return "{\"error\":\"appid and source are both required\"}";
            LOG_INFO("LuaFlipperUI: installing {} from {}", appId, source);
            return LuaFlipperDownload::Install(appId, source, g_steamPath);
        }

        if (path == "/api/unlocker")  return JsonUnlocker();
        // Everything else lives in LuaFlipperPages.
        return LuaFlipperPages::Render(path, g_steamPath);
    }

    /* ------------------------------------------------------------ threads --- */

    void ServerThread() {
        int srv = ::socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) { LOG_ERROR("LuaFlipperUI: socket() failed"); return; }
        int one = 1;
        ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kApiPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only, never 0.0.0.0
        if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            ::listen(srv, 8) != 0) {
            LOG_ERROR("LuaFlipperUI: cannot listen on 127.0.0.1:{}", kApiPort);
            ::close(srv);
            return;
        }
        LOG_INFO("LuaFlipperUI: API listening on 127.0.0.1:{}", kApiPort);

        for (;;) {
            int c = ::accept(srv, nullptr, nullptr);
            if (c < 0) continue;

            // Handled on this thread, one at a time, on purpose.
            //
            // A thread per connection is the obvious speed-up and did cut five
            // parallel requests from 122/426/684/1363/1365 ms to
            // 2/282/380/424/620 ms. It also killed the Steam client twice.
            // Http::Execute makes a fresh easy handle per call, but nothing here
            // calls curl_global_init, so libcurl runs it implicitly inside the
            // first curl_easy_init, and that implicit path is explicitly not
            // thread-safe. Several routes proxy Steam, so concurrent handling
            // races there on startup. LuaConfig's readers are not audited for
            // concurrent use either.
            //
            // Serialising costs latency in a background page; the alternative
            // crashes the client the module is loaded into. Revisit by calling
            // curl_global_init once at load and auditing LuaConfig, not by
            // reintroducing the thread on its own.
            {
                std::string req;
                char buf[4096];
                ssize_t r = ::recv(c, buf, sizeof(buf), 0);
                if (r > 0) req.assign(buf, static_cast<size_t>(r));

                std::string path;
                size_t sp = req.find(' ');
                if (sp != std::string::npos) {
                    size_t sp2 = req.find(' ', sp + 1);
                    if (sp2 != std::string::npos)
                        path = req.substr(sp + 1, sp2 - sp - 1);
                }

                const std::string body = RouteApi(path);
                const bool found = !body.empty();
                std::string resp =
                    std::string("HTTP/1.1 ") + (found ? "200 OK" : "404 Not Found") +
                    "\r\nContent-Type: application/json\r\n"
                    // The UI runs on steamloopback.host, so every call here is
                    // cross-origin; without this the browser blocks the response
                    // and the page reports a fetch failure even though the
                    // server replied.
                    "Access-Control-Allow-Origin: *\r\n"
                    "Cache-Control: no-store\r\n"
                    "Content-Length: " + std::to_string(found ? body.size() : 2) +
                    "\r\nConnection: close\r\n\r\n" + (found ? body : "{}");
                SendAll(c, resp.data(), resp.size());
                ::close(c);
            }
        }
    }

    void InjectorThread() {
        const std::filesystem::path uiDir =
            std::filesystem::path(g_steamPath) / "steamflipper" / "ui";
        const std::string js    = ReadWholeFile(uiDir / "luaflipper.js");
        const std::string css   = ReadWholeFile(uiDir / "luaflipper.css");
        // Page renderers. Optional: luaflipper.js falls back to a built-in
        // renderer when this is absent, so a partial install still shows a UI.
        const std::string pages = ReadWholeFile(uiDir / "luaflipper.pages.js");
        if (js.empty()) {
            LOG_ERROR("LuaFlipperUI: no UI assets at {}", uiDir.string());
            return;
        }

        std::string lastTarget;
        bool haveClasses = false;
        // Each distinct failure is reported once. Without this the loop is
        // silent when it never succeeds, which is exactly when a log is needed.
        int reported = 0;
        auto once = [&](int bit, const char* what) {
            if (reported & bit) return;
            reported |= bit;
            LOG_ERROR("LuaFlipperUI: {}", what);
        };

        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            const std::string list = HttpGet(kCdpPort, "/json/list");
            if (list.empty()) {
                once(1, "no reply from the CEF debugger on 127.0.0.1:8080; is "
                        ".cef-enable-remote-debugging present and Steam restarted?");
                continue;
            }

            // The nav lives in the main client window. SharedJSContext holds an
            // effectively empty document and is the wrong target.
            size_t at = list.find("\"Steam\"");
            if (at == std::string::npos) at = list.find("createflags=18");
            if (at == std::string::npos) {
                once(2, "CEF is up but the main client window target was not "
                        "found in /json/list");
                continue;
            }
            const std::string ws = JsonString(list, "webSocketDebuggerUrl", at);
            if (ws.empty()) {
                once(4, "main window target has no webSocketDebuggerUrl");
                continue;
            }
            if (ws == lastTarget && g_injected) {
                // Steam's menus are empty documents until they have been opened
                // once, so a class read at startup comes back blank and the UI
                // falls back to its own styling. Keep retrying, and push the
                // real names in as soon as they exist rather than reinjecting.
                if (!haveClasses) {
                    const std::string k = ReadSteamMenuClasses(list);
                    if (!k.empty()) {
                        WebSocket s;
                        if (s.Open(ws)) {
                            s.Call("Runtime.evaluate",
                                   "{\"expression\":\"" +
                                   JsonEscape("window.__luaflipperMenuClasses=" + k) +
                                   "\"}");
                            s.Close();
                            haveClasses = true;
                            LOG_INFO("LuaFlipperUI: adopted Steam's menu classes");
                        }
                    }
                }
                continue;   // already injected
            }

            std::string bootstrap;
            const std::string klass = ReadSteamMenuClasses(list);
            if (!klass.empty())
                bootstrap += "window.__luaflipperMenuClasses=" + klass + ";\n";
            bootstrap +=
                "(function(){var s=document.createElement('style');"
                "s.setAttribute('data-luaflipper','1');s.textContent=\"" +
                JsonEscape(css) + "\";document.head.appendChild(s);})();\n";
            // Renderers must define window.LUAFlipperPages before the main
            // script opens a page.
            if (!pages.empty()) bootstrap += pages + "\n";
            bootstrap += js;

            if (InjectInto(ws, bootstrap)) {
                lastTarget = ws;
                g_injected = true;
                LOG_INFO("LuaFlipperUI: injected into {}{}", ws,
                         klass.empty() ? " (fallback styling)" : " (steam menu classes)");
            } else {
                once(8, "websocket upgrade to the main window target failed");
            }
        }
    }

#endif // __linux__

} // namespace

void Initialize(const char* steamInstallPath) {
#if defined(__linux__)
    if (!Config::GetUiEnabled()) {
        LOG_INFO("LuaFlipperUI: [ui].enabled is false, client UI disabled");
        return;
    }
    g_steamPath = steamInstallPath ? steamInstallPath : "";

    // Steam only opens its CEF debugger when this marker exists, and it is the
    // only channel a standalone injector has into the frontend.
    const auto marker = std::filesystem::path(g_steamPath) / ".cef-enable-remote-debugging";
    std::error_code ec;
    if (!std::filesystem::exists(marker, ec)) {
        LOG_INFO("LuaFlipperUI: {} absent, client UI disabled until next Steam "
                 "restart (the installer creates it)", marker.string());
        std::ofstream(marker).close();
        return;
    }

    std::thread(ServerThread).detach();
    std::thread(InjectorThread).detach();
#else
    (void)steamInstallPath;
#endif
}

} // namespace LuaFlipperUI
