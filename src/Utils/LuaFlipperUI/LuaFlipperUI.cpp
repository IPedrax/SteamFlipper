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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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

    // Numeric field, same scope as JsonString. -1 when absent.
    long long JsonNumber(const std::string& doc, const std::string& key, size_t from = 0) {
        const std::string needle = "\"" + key + "\"";
        size_t k = doc.find(needle, from);
        if (k == std::string::npos) return -1;
        size_t c = doc.find(':', k + needle.size());
        if (c == std::string::npos) return -1;
        return strtoll(doc.c_str() + c + 1, nullptr, 10);
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
        // Recv() returns empty both when the read timed out and when the peer
        // went away. A long-lived session has to tell those apart: one means
        // "nothing happened", the other means the target is gone.
        bool closed = false;

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

        // Send a command without waiting for anything. Used where the reply is
        // not wanted and draining one frame would swallow an event instead.
        bool Post(const std::string& method, const std::string& paramsJson) {
            std::string msg = "{\"id\":" + std::to_string(++nextId) +
                              ",\"method\":\"" + method + "\"";
            if (!paramsJson.empty()) msg += ",\"params\":" + paramsJson;
            msg += "}";
            return Send(msg);
        }

        bool Call(const std::string& method, const std::string& paramsJson) {
            if (!Post(method, paramsJson)) return false;
            // Drain one frame so replies do not pile up in the socket buffer.
            Recv();
            return true;
        }

        void SetRecvTimeout(int seconds) {
            timeval tv{};
            tv.tv_sec = seconds;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        std::string Recv() {
            auto need = [&](size_t n) -> bool {
                char buf[8192];
                while (rest.size() < n) {
                    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
                    if (r == 0) { closed = true; return false; }
                    if (r < 0) {
                        // A timeout leaves whatever arrived in `rest`, so the
                        // next call resumes the same frame rather than losing it.
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            closed = true;
                        return false;
                    }
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

    /**
     * Every debuggable target currently showing a Steam store page.
     *
     * The client renders the store by pointing one of its browser views at
     * store.steampowered.com, so the store is a target of its own rather than a
     * route inside the main window's React app. Big Picture does the same with a
     * different view, which is why this matches on the URL and not on a title:
     * one rule covers both, and covers however many views exist at once.
     *
     * Entries are emitted with `url` ahead of `webSocketDebuggerUrl`, so the
     * first debugger URL after a matching page URL belongs to that entry.
     */
    std::vector<std::string> StoreTargets(const std::string& listJson) {
        std::vector<std::string> out;
        const std::string key = "\"url\":";
        for (size_t at = listJson.find(key); at != std::string::npos;
             at = listJson.find(key, at + key.size())) {
            const std::string url = JsonString(listJson, "url", at);
            if (url.find("://store.steampowered.com") == std::string::npos) continue;
            const std::string ws = JsonString(listJson, "webSocketDebuggerUrl", at);
            if (!ws.empty()) out.push_back(ws);
        }
        return out;
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

    /**
     * Manifests taken out of service but not yet gone.
     *
     * Remove renames rather than unlinks so the removal can be undone, and the
     * loader ignores these because it collects `.lua` and these end in
     * `.removed`. They are swept at the next Steam start, which is what bounds
     * the undo to the session that did the removing.
     */
    std::vector<std::filesystem::path> RemovedFiles() {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        for (const std::string& dir : LuaConfig::MergeWatchDirs(
                 Config::GetLuaPaths(), (std::filesystem::path(g_steamPath) /
                                         "config" / "stplug-in").string())) {
            for (auto it = std::filesystem::directory_iterator(dir, ec);
                 !ec && it != std::filesystem::directory_iterator(); ++it) {
                if (it->is_regular_file(ec) &&
                    it->path().extension() == ".removed")
                    out.push_back(it->path());
            }
        }
        return out;
    }

    /**
     * Delete every tombstone left by a previous session.
     *
     * Runs once at startup, which is the moment the undo they exist for expires
     * anyway: the loader has just re-read the directory, so a manifest that is
     * still `.removed` is one the user did not put back, and ownership for it
     * is gone from this Steam either way.
     */
    void SweepRemoved() {
        std::error_code ec;
        size_t gone = 0;
        for (const auto& p : RemovedFiles()) {
            if (std::filesystem::remove(p, ec) && !ec) gone++;
        }
        if (gone)
            LOG_INFO("LuaFlipperUI: deleted {} manifest(s) removed last session",
                     gone);
    }

    // Ownership entries and how many of them carry a decryption key. A file
    // with no keyed line registers ownership it cannot decrypt a byte of.
    void CountAppIds(const std::string& body, size_t& ids, size_t& keys) {
        ids = 0;
        keys = 0;
        for (size_t i = body.find("addappid("); i != std::string::npos;
             i = body.find("addappid(", i + 1)) {
            // A keyed entry is addappid(id, 1, "<64 hex>").
            size_t end = body.find(')', i);
            if (end == std::string::npos) break;
            ids++;
            if (body.find('"', i) < end) keys++;
        }
    }

    std::string JsonManifests() {
        std::string j = "{\"manifests\":[";
        bool first = true;
        auto emit = [&](const std::filesystem::path& p,
                        const std::string& appId, bool removed) {
            size_t ids = 0, keys = 0;
            CountAppIds(ReadWholeFile(p), ids, keys);
            j += first ? "" : ",";
            first = false;
            j += "{\"file\":\"" + JsonEscape(p.filename().string()) + "\"";
            j += ",\"appid\":\"" + JsonEscape(appId) + "\"";
            j += ",\"ids\":" + std::to_string(ids);
            j += ",\"keys\":" + std::to_string(keys);
            j += ",\"removed\":" + std::string(removed ? "true" : "false") + "}";
        };

        for (const auto& p : LuaFiles()) emit(p, p.stem().string(), false);

        // Listed alongside the live ones so the page can offer to put them
        // back. Out of service either way: the loader never saw them.
        for (const auto& p : RemovedFiles())
            emit(p, std::filesystem::path(p.stem()).stem().string(), true);

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
        // library_capsule is the portrait art the client's own library draws
        // with, and the only address that resolves for it: the flat
        // .../steam/apps/<id>/library_600x900.jpg path predates the hashed
        // assets and 404s for most of what gets added here.
        const char* keys[] = { "header", "library_hero", "page_background",
                               "main_capsule", "small_capsule", "hero_capsule",
                               "library_capsule" };
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

    /* ------------------------------------------------------- config write --- */

    /**
     * Set one key in steamflipper.toml, in place.
     *
     * Deliberately a line edit rather than a parse-and-reserialise. toml++ can
     * write a document back out, but it would come back stripped of the comments
     * that explain every setting in that file, and those comments are most of
     * what makes it editable by hand. So the section is found, the key inside it
     * is rewritten if present and appended if not, and everything else is
     * untouched.
     *
     * The file is hot-reloaded, so a write lands without a restart for anything
     * read through Config::Get*.
     */
    bool WriteConfigKey(const std::string& section, const std::string& key,
                        const std::string& rawValue, std::string& err) {
        const std::filesystem::path path =
            std::filesystem::path(g_steamPath) / "steamflipper.toml";
        std::string body = ReadWholeFile(path);

        const std::string header = "[" + section + "]";
        const size_t sec = body.find(header);

        if (sec == std::string::npos) {
            if (!body.empty() && body.back() != '\n') body += "\n";
            body += "\n" + header + "\n" + key + " = " + rawValue + "\n";
        } else {
            // The section runs to the next header or to EOF. Staying inside it
            // matters: several sections could carry a key of the same name.
            size_t end = body.find("\n[", sec + header.size());
            end = (end == std::string::npos) ? body.size() : end + 1;

            size_t at = std::string::npos;
            for (size_t i = body.find(key, sec); i < end && i != std::string::npos;
                 i = body.find(key, i + 1)) {
                // Start of a line, and the key is the whole word before '='.
                const size_t lineStart = body.rfind('\n', i);
                const std::string before =
                    body.substr(lineStart + 1, i - lineStart - 1);
                if (before.find_first_not_of(" \t") != std::string::npos) continue;
                const size_t eq = body.find_first_not_of(" \t", i + key.size());
                if (eq != std::string::npos && body[eq] == '=') { at = i; break; }
            }

            if (at == std::string::npos) {
                body.insert(end, key + " = " + rawValue + "\n");
            } else {
                size_t lineEnd = body.find('\n', at);
                if (lineEnd == std::string::npos) lineEnd = body.size();
                body.replace(at, lineEnd - at, key + " = " + rawValue);
            }
        }

        // Written through a temp file in the same directory so a crash mid-write
        // cannot leave the config truncated, which would silently reset every
        // setting in it on the next read.
        const std::filesystem::path tmp = path.string() + ".sfnew";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { err = "cannot write to " + tmp.string(); return false; }
            f << body;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) { err = ec.message(); std::filesystem::remove(tmp, ec); return false; }

        // The file can now hold a credential, so it stops being world-readable.
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
        return true;
    }

    // A TOML basic string. The values that reach here are an API key and source
    // names, but quoting is not the caller's job to remember.
    std::string TomlString(const std::string& v) {
        std::string out = "\"";
        for (char c : v) {
            if (c == '"' || c == '\\') out += '\\';
            if (c == '\n' || c == '\r') continue;
            out += c;
        }
        return out + "\"";
    }

    /* ------------------------------------------------------------- hubcap --- */

    /**
     * What this Hubcap key is worth today.
     *
     * Free: the stats endpoint does not consume the daily allowance it reports.
     * Passed through mostly as Hubcap sends it, because these are its numbers
     * about the user's own key and restating them here would only add a place
     * for the two to disagree.
     */
    // The configured order, so the page can draw the list it edits. Emitted
    // alongside every hubcap answer because the two are one screen.
    std::string SourceOrderJson() {
        std::string j = ",\"order\":[";
        bool first = true;
        // The effective order, not the configured one: a source this build
        // knows but nobody has ranked yet still belongs on the page.
        for (const std::string& n : LuaFlipperDownload::EffectiveOrder()) {
            j += first ? "" : ",";
            first = false;
            j += "\"" + JsonEscape(n) + "\"";
        }
        return j + "]";
    }

    /*
     * The key itself is never sent back.
     *
     * The page only needs to know whether one is set and what it is worth; the
     * value would be a credential travelling to a browser view for no reason,
     * and any local process that can reach this port would get it for free.
     * Replacing a key means typing the new one, which is what a settings screen
     * does anyway.
     */
    // Shape check, shared by everything that can be handed a key.
    bool HubcapKeyShaped(const std::string& k) {
        if (k.size() != 100 || k.compare(0, 4, "smm_") != 0) return false;
        for (size_t i = 4; i < k.size(); i++) {
            const char c = k[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    /**
     * What Hubcap says about one key.
     *
     * Takes the key rather than reading it from config, because the caller that
     * has just saved one cannot read it back yet: the file is picked up by a
     * watcher, so a check that went through config would race the reload and
     * report a good key as rejected. That is exactly what it did.
     */
    std::string HubcapStatsFor(const std::string& key) {
        if (key.empty())
            return "{\"configured\":false" + SourceOrderJson() + "}";
        if (!HubcapKeyShaped(key))
            return "{\"configured\":true,\"valid\":false,\"error\":\"A Hubcap key is "
                   "\\\"smm_\\\" followed by 96 hex characters.\"" +
                   SourceOrderJson() + "}";


        const std::string url =
            "https://hubcapmanifest.com/api/v1/user/stats?api_key=" + key;
        auto r = SFPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, nullptr,
                                           5000, 5000, 10000, 15000, 16u * 1024u);
        if (r.ok && r.status == 401)
            return "{\"configured\":true,\"valid\":false,\"error\":\"Hubcap rejected "
                   "this key.\"" + SourceOrderJson() + "}";
        if (!r.ok || r.status != 200)
            return "{\"configured\":true,\"valid\":false,\"error\":\"Hubcap is "
                   "unreachable.\"" + SourceOrderJson() + "}";

        std::string j = "{\"configured\":true,\"valid\":true";
        j += ",\"used\":"    + std::to_string(JsonNumber(r.body, "daily_usage"));
        j += ",\"limit\":"   + std::to_string(JsonNumber(r.body, "daily_limit"));
        j += ",\"expires\":\"" +
             JsonEscape(JsonString(r.body, "api_key_expires_at")) + "\"";
        // can_make_requests is Hubcap's own verdict, and it can be false while
        // used < limit (an expired key, for one). Reported rather than derived.
        j += ",\"canRequest\":" +
             std::string(r.body.find("\"can_make_requests\":true") != std::string::npos
                         ? "true" : "false");
        j += SourceOrderJson();
        j += "}";
        return j;
    }

    std::string JsonHubcapStats() {
        return HubcapStatsFor(Config::GetHubcapKey());
    }

    /**
     * Save the Hubcap key from the page.
     *
     * Shape-checked before it is written: the file is hot-reloaded, so a bad
     * value would be live immediately and every later error would be about a
     * request rather than about the typo that caused it. An empty value clears
     * the key, which is the only way to take the source back out from here.
     */
    std::string JsonHubcapSave(const std::string& fullPath) {
        const std::string key = QueryParam(fullPath, "key");

        if (!key.empty() && !HubcapKeyShaped(key))
            return "{\"error\":\"A Hubcap key is \\\"smm_\\\" followed by 96 "
                   "hex characters.\"}";

        std::string err;
        if (!WriteConfigKey("hubcap", "key", TomlString(key), err))
            return "{\"error\":\"Could not save: " + JsonEscape(err) + "\"}";
        LOG_INFO("LuaFlipperUI: hubcap key {}", key.empty() ? "cleared" : "saved");

        // The verdict comes back with the save, checked against the key that was
        // just handed over rather than the one config has caught up to. One
        // request, one answer, and no window in which a good key reads as bad.
        std::string j = "{\"ok\":true,\"cleared\":" +
                        std::string(key.empty() ? "true" : "false") + ",\"stats\":";
        j += HubcapStatsFor(key);
        j += "}";
        return j;
    }

    /**
     * Save the order sources are tried in.
     *
     * Names are not validated against the built-in set. A source this build does
     * not know is simply never matched when the probe orders itself, and one
     * left out of the list is tried last rather than dropped, so the worst a bad
     * list can do is fail to reorder.
     */
    std::string JsonSourcesSave(const std::string& fullPath) {
        const std::string order = QueryParam(fullPath, "order");

        std::string arr = "[";
        size_t at = 0;
        bool first = true;
        while (at <= order.size()) {
            const size_t comma = order.find(',', at);
            const std::string name = order.substr(
                at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!name.empty()) {
                arr += first ? "" : ", ";
                first = false;
                arr += TomlString(name);
            }
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        arr += "]";

        std::string err;
        if (!WriteConfigKey("sources", "order", arr, err))
            return "{\"error\":\"Could not save: " + JsonEscape(err) + "\"}";
        LOG_INFO("LuaFlipperUI: source order saved as {}", arr);
        return "{\"ok\":true}";
    }

    /* ------------------------------------------------------------ nav menu --- */

    /**
     * Evaluate one expression in a CEF target picked by a marker in /json/list.
     *
     * Short-lived on purpose. The nav menu is three calls a second at worst,
     * each a few milliseconds on loopback, and a connection held open per
     * target would be three more things to notice dying.
     */
    bool EvalInTarget(const std::string& marker, const std::string& expr,
                      std::string* value = nullptr) {
        const std::string list = HttpGet(kCdpPort, "/json/list");
        const size_t at = list.find(marker);
        if (at == std::string::npos) return false;
        const std::string ws = JsonString(list, "webSocketDebuggerUrl", at);
        if (ws.empty()) return false;

        WebSocket s;
        if (!s.Open(ws)) return false;
        s.Post("Runtime.evaluate",
               "{\"expression\":\"" + JsonEscape(expr) + "\",\"returnByValue\":true}");
        // Read the reply when the caller wants the answer rather than just
        // delivery. Getting that distinction wrong is what made the client
        // window think a menu was up when nothing had been drawn.
        const std::string reply = value ? s.Recv() : (s.Recv(), std::string());
        s.Close();
        if (value) {
            const size_t r = reply.find("\"result\"");
            *value = (r == std::string::npos) ? "" : JsonString(reply, "value", r);
            if (value->empty() && reply.find("\"value\":true") != std::string::npos)
                *value = "true";
        }
        return true;
    }

    // The dropdown, in the order the client window draws it. Kept here so the
    // popup script has no opinion about what the menu contains.
    constexpr const char* kMenuItems =
        "[{page:'unlocker',label:'Unlocker'},{page:'manage',label:'Manage'},"
        "{page:'fixes',label:'Fixes'},{page:'config',label:'Config'}]";

    /**
     * Show the dropdown as a real Steam window.
     *
     * x and y arrive as client coordinates of the tab in the client window;
     * the popup script adds that window's screen origin, which only it can ask
     * for. Returns whether the call was delivered, not whether a menu appeared:
     * the client window falls back to its in-page menu on a false, and the
     * popup script answers false itself when Steam's internals are not there.
     */
    std::string JsonNavMenuShow(const std::string& fullPath) {
        const std::string x = QueryParam(fullPath, "x");
        const std::string y = QueryParam(fullPath, "y");
        if (x.empty() || y.empty()) return "{\"ok\":false}";
        // Digits only: these go straight into an expression.
        for (const std::string* v : { &x, &y }) {
            if (v->find_first_not_of("0123456789") != std::string::npos)
                return "{\"ok\":false}";
        }
        // The helper answers with its own rectangle as a JSON string, or false.
        // Passed through rather than reduced to a boolean: the client window
        // needs those coordinates to tell movement over the popup apart from
        // movement over itself.
        // Off means the in-page menu, which is what the client window does when
        // this answers false.
        if (!Config::GetUiPopupMenu()) return "{\"ok\":false}";

        const std::string expr =
            "String((window.__luaflipperMenu && window.__luaflipperMenu.show(" +
            x + "," + y + "," + kMenuItems + ")) || false)";
        std::string got;
        if (!EvalInTarget("\"SharedJSContext\"", expr, &got) || got.empty() ||
            got.compare(0, 1, "{") != 0)
            return "{\"ok\":false}";
        return got;
    }

    // The pointer moved onto the popup. Cancels the close the tab scheduled
    // when the pointer left it, which is the same movement seen from the
    // other side.
    std::string JsonNavMenuKeep() {
        EvalInTarget("\"Steam\"",
                     "window.__luaflipperMenuKeep&&window.__luaflipperMenuKeep()");
        return "{\"ok\":true}";
    }

    // And left it, for somewhere that is not the tab either.
    std::string JsonNavMenuClose() {
        LOG_INFO("LuaFlipperUI: nav menu closed (the popup lost the pointer)");
        EvalInTarget("\"Steam\"",
                     "window.__luaflipperMenuClose&&window.__luaflipperMenuClose()");
        return "{\"ok\":true}";
    }

    /*
     * Every dismissal says who asked for it.
     *
     * This menu has been fixed four times against reproductions that all
     * passed, because injected pointer events are delivered differently from a
     * real hand and the difference is exactly what the bug lives in. So the
     * next report does not need a theory: the log names the caller, and the
     * callers are few enough that the name is the answer.
     */
    std::string JsonNavMenuHide(const std::string& fullPath) {
        const std::string why = QueryParam(fullPath, "why");
        LOG_INFO("LuaFlipperUI: nav menu hidden ({})",
                 why.empty() ? "unspecified" : why);
        EvalInTarget("\"SharedJSContext\"",
                     "window.__luaflipperMenu&&window.__luaflipperMenu.hide()");
        return "{\"ok\":true}";
    }

    /**
     * A click in the popup, handed to the window that owns the tab.
     *
     * The page name is checked against the menu rather than passed through: it
     * arrives from a document that any script in SharedJSContext could reach,
     * and it ends up inside an expression evaluated in the client window.
     */
    std::string JsonNavMenuPick(const std::string& fullPath) {
        const std::string page = QueryParam(fullPath, "page");
        if (page != "unlocker" && page != "manage" &&
            page != "fixes" && page != "config")
            return "{\"error\":\"unknown page\"}";

        EvalInTarget("\"Steam\"",
                     "window.__luaflipperOpen&&window.__luaflipperOpen('" + page + "')");
        return "{\"ok\":true}";
    }

    /* --------------------------------------------------------- steam urls --- */

    /**
     * Catch the website's "Activate in HubcapTools" button.
     *
     * That button is an <a href="steam://hubcaptools/setapikey/KEY">. The verb
     * belongs to HubcapTools, a DLL proxy of the same shape as this one that
     * registers a handler for it, which is why the page says it needs that tool
     * installed and Steam running. Nothing published lets a second proxy claim
     * a verb, and the JS side does not see it either: RegisterForRunSteamURL in
     * SharedJSContext never fires for a verb the client has no handler for.
     *
     * But Steam writes every URL it dispatches to its own console log first:
     *
     *   ExecuteSteamURL: "steam://hubcaptools/setapikey/smm_..."
     *
     * so the line is there whether or not anything handles it. Tailing that is
     * not elegant, and it is the only route from here that does not mean
     * hooking an address that changes every client build - the same fragility
     * that has already cost this project two features.
     *
     * Only lines appended after this starts are read. The log holds every key
     * ever activated, and re-applying an old one on every launch would quietly
     * undo a key the user changed by hand.
     */
    void SteamUrlWatcher() {
        const std::filesystem::path log =
            std::filesystem::path(g_steamPath) / "logs" / "console_log.txt";

        // Where to resume from. Starting at the current end skips the history;
        // a log that gets rotated or truncated resets to its new end rather
        // than replaying it.
        std::error_code ec;
        uintmax_t at = std::filesystem::file_size(log, ec);
        if (ec) at = 0;

        const std::string marks[] = {
            "ExecuteSteamURL: \"steam://hubcaptools/setapikey/",
            "ExecuteSteamURL: \"steam://steamflipper/setapikey/",
        };

        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));

            const uintmax_t size = std::filesystem::file_size(log, ec);
            if (ec) continue;
            if (size < at) { at = size; continue; }     // rotated
            if (size == at) continue;

            std::ifstream f(log, std::ios::binary);
            if (!f) continue;
            f.seekg(static_cast<std::streamoff>(at));
            std::string chunk((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
            at = size;

            for (const std::string& mark : marks) {
                for (size_t i = chunk.find(mark); i != std::string::npos;
                     i = chunk.find(mark, i + 1)) {
                    const size_t from = i + mark.size();
                    const size_t end = chunk.find('"', from);
                    if (end == std::string::npos) continue;
                    const std::string key = chunk.substr(from, end - from);
                    if (!HubcapKeyShaped(key)) {
                        LOG_WARN("LuaFlipperUI: setapikey URL carried something "
                                 "that is not a Hubcap key, ignoring");
                        continue;
                    }
                    if (key == Config::GetHubcapKey()) continue;   // already ours

                    std::string err;
                    if (WriteConfigKey("hubcap", "key", TomlString(key), err))
                        LOG_INFO("LuaFlipperUI: adopted the Hubcap key from a "
                                 "setapikey URL");
                    else
                        LOG_WARN("LuaFlipperUI: could not save the key from a "
                                 "setapikey URL: {}", err);
                }
            }
        }
    }

    /* --------------------------------------------------------- lua.tools --- */

    // Defined with the unlocker-mode lease further down; both want a clock that
    // cannot be moved by the user changing the system time.
    long long SteadyMs();

    /*
     * A lua.tools session, from a Discord code.
     *
     * Their catalog is public; downloading a fix is not. The credential behind
     * it is a Supabase session, and the sign-in that suits a program with no
     * browser is their Discord bot route: the user runs /login with the bot,
     * gets a six-character code, and types it here. The code is the credential,
     * single-use with a five-minute life, and it never was a password.
     *
     * Two exchanges turn it into a session, then the refresh token is the only
     * thing worth keeping: access tokens expire within the hour, which is why
     * pasting one into the config file was never going to work.
     */
    constexpr const char* kSupabase = "https://db.lua.tools";

    // Public by design: it identifies the project to Supabase and grants
    // nothing on its own. It ships in the lua.tools web bundle too.
    constexpr const char* kSupabaseAnon =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpYXQiOjE3NzYwMzkzNzYsImV4cCI6MTg5"
        "MzQ1NjAwMCwicm9sZSI6ImFub24iLCJpc3MiOiJzdXBhYmFzZSJ9."
        "f_-K38u3odjltP-g_67FVmG32Vg-_-k-lNBvIaVUVBM";

    // Their edge refuses anything that does not look like a browser with a bare
    // 403 and no body worth reading, so every call here carries one.
    constexpr const wchar_t* kBrowserUA =
        L"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:154.0) "
        L"Gecko/20100101 Firefox/154.0\r\n";

    std::mutex g_sessionLock;
    std::string g_accessToken;          // in memory only; expires
    long long   g_accessExpires = 0;    // steady ms

    SFPlatform::Http::Result PostJson(const std::string& url, const std::string& body,
                                      const std::wstring& extraHeaders) {
        const std::wstring headers =
            std::wstring(L"Content-Type: application/json\r\n") + kBrowserUA + extraHeaders;
        return SFPlatform::Http::Execute(L"POST", url.c_str(), body.data(),
                                         static_cast<uint32_t>(body.size()),
                                         headers.c_str(), 5000, 5000, 10000, 20000,
                                         256u * 1024u);
    }

    /**
     * Swap the stored refresh token for a usable access token.
     *
     * Cached until shortly before it expires, and the refresh token is rewritten
     * every time because Supabase rotates it on each exchange: keeping the old
     * one would sign the user out at the next restart.
     */
    std::string FixesAccessToken(std::string& err) {
        {
            std::lock_guard<std::mutex> lock(g_sessionLock);
            if (!g_accessToken.empty() && g_accessExpires > SteadyMs() + 60000)
                return g_accessToken;
        }

        const std::string refresh = Config::GetFixesRefreshToken();
        if (refresh.empty()) {
            // A token pasted in by hand still works, for as long as it lives.
            const std::string manual = Config::GetFixesToken();
            if (!manual.empty()) return manual;
            err = "not signed in";
            return {};
        }

        auto r = PostJson(std::string(kSupabase) + "/auth/v1/token?grant_type=refresh_token",
                          "{\"refresh_token\":\"" + JsonEscape(refresh) + "\"}",
                          std::wstring(L"apikey: ") +
                          std::wstring(std::string(kSupabaseAnon).begin(),
                                       std::string(kSupabaseAnon).end()) + L"\r\n");
        if (!r.ok || r.status != 200) {
            err = "the saved session was refused; sign in again";
            return {};
        }

        const std::string access = JsonString(r.body, "access_token");
        const std::string rotated = JsonString(r.body, "refresh_token");
        if (access.empty()) { err = "no access token in the reply"; return {}; }

        if (!rotated.empty() && rotated != refresh) {
            std::string werr;
            WriteConfigKey("fixes", "refresh_token", TomlString(rotated), werr);
        }
        {
            std::lock_guard<std::mutex> lock(g_sessionLock);
            g_accessToken = access;
            const long long ttl = JsonNumber(r.body, "expires_in");
            g_accessExpires = SteadyMs() + (ttl > 0 ? ttl : 3600) * 1000;
        }
        return access;
    }

    /**
     * Sign in through the browser, the way their own desktop client does.
     *
     * The Discord bot code works, but only if you know to run /login with a bot
     * in a Discord server, which is a thing nobody finds on their own. This is
     * the other route their client offers: Discord's normal authorise page in
     * the real browser, and no code to copy.
     *
     * It works here because the module already runs a loopback server, so it
     * can be the landing point. Verified against their Supabase before building
     * it: authorize?redirect_to=http://127.0.0.1:1987/... comes back 302 to
     * Discord with that redirect intact, so the project's allow-list permits it.
     *
     * PKCE, so the code that lands in the URL is worth nothing without the
     * verifier, which never leaves this process.
     */
    std::string g_pkceVerifier;

    // The redirect goes in a query string, so it is escaped there.
    std::string UrlEncodeUi(const std::string& in) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : in) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0x0F];
            }
        }
        return out;
    }

    std::string Base64Url(const std::string& raw) {
        static const char* abc =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        for (size_t i = 0; i < raw.size(); i += 3) {
            const unsigned a = static_cast<unsigned char>(raw[i]);
            const unsigned b = i + 1 < raw.size() ? static_cast<unsigned char>(raw[i+1]) : 0;
            const unsigned c = i + 2 < raw.size() ? static_cast<unsigned char>(raw[i+2]) : 0;
            const unsigned n = (a << 16) | (b << 8) | c;
            out += abc[(n >> 18) & 63];
            out += abc[(n >> 12) & 63];
            if (i + 1 < raw.size()) out += abc[(n >> 6) & 63];
            if (i + 2 < raw.size()) out += abc[n & 63];
        }
        return out;   // no padding, which is what base64url wants here
    }

    std::string HexToRaw(const std::string& hex) {
        std::string out;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            out += static_cast<char>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        return out;
    }

    /** The URL to open, and the verifier kept behind for the exchange. */
    std::string JsonFixesSignIn() {
        // Random enough for a one-shot verifier: the value only has to be
        // unguessable for the seconds between opening the browser and the
        // redirect coming back.
        std::string seed = std::to_string(SteadyMs()) + "|" +
                           std::to_string(reinterpret_cast<uintptr_t>(&seed)) + "|" +
                           std::to_string(::getpid());
        {
            std::ifstream ur("/dev/urandom", std::ios::binary);
            if (ur) { char b[32]; ur.read(b, sizeof(b)); seed.append(b, ur.gcount()); }
        }
        const std::string verifier =
            Base64Url(HexToRaw(SFPlatform::Hash::Sha256OfBuffer(seed.data(), seed.size())));
        const std::string challenge = Base64Url(HexToRaw(
            SFPlatform::Hash::Sha256OfBuffer(verifier.data(), verifier.size())));

        {
            std::lock_guard<std::mutex> lock(g_sessionLock);
            g_pkceVerifier = verifier;
        }

        const std::string redirect = "http://127.0.0.1:1987/api/fixes/callback";
        std::string url = std::string(kSupabase) +
            "/auth/v1/authorize?provider=discord&redirect_to=" + UrlEncodeUi(redirect) +
            "&code_challenge=" + challenge + "&code_challenge_method=s256";
        return "{\"ok\":true,\"url\":\"" + JsonEscape(url) + "\"}";
    }

    /** Where the browser lands once Discord is done. */
    std::string JsonFixesCallback(const std::string& fullPath) {
        auto page = [](const std::string& head, const std::string& body) {
            return "<html><head><meta charset=\"utf-8\"><title>LUAFlipper</title>"
                   "</head><body style=\"background:#1b2838;color:#dcdedf;"
                   "font:15px/1.5 Arial,sans-serif;padding:56px;text-align:center\">"
                   "<h2 style=\"color:#66c0f4\">" + head + "</h2><p>" + body +
                   "</p></body></html>";
        };

        const std::string err = QueryParam(fullPath, "error_description");
        if (!err.empty())
            return page("Sign-in was refused", JsonEscape(err));

        const std::string code = QueryParam(fullPath, "code");
        if (code.empty())
            return page("Nothing to exchange",
                        "Discord sent no code back. Close this and try again.");

        std::string verifier;
        {
            std::lock_guard<std::mutex> lock(g_sessionLock);
            verifier = g_pkceVerifier;
            g_pkceVerifier.clear();      // one shot
        }
        if (verifier.empty())
            return page("This link has expired",
                        "Start the sign-in from the LUAFlipper tab again.");

        const std::string anon(kSupabaseAnon);
        auto r = PostJson(std::string(kSupabase) + "/auth/v1/token?grant_type=pkce",
                          "{\"auth_code\":\"" + JsonEscape(code) +
                          "\",\"code_verifier\":\"" + JsonEscape(verifier) + "\"}",
                          std::wstring(L"apikey: ") +
                          std::wstring(anon.begin(), anon.end()) + L"\r\n");
        if (!r.ok || r.status != 200)
            return page("The session was refused",
                        "lua.tools would not exchange that sign-in.");

        const std::string refresh = JsonString(r.body, "refresh_token");
        const std::string access  = JsonString(r.body, "access_token");
        if (refresh.empty())
            return page("No session came back", "Try again from the LUAFlipper tab.");

        std::string werr;
        WriteConfigKey("fixes", "refresh_token", TomlString(refresh), werr);
        {
            std::lock_guard<std::mutex> lock(g_sessionLock);
            g_accessToken = access;
            const long long ttl = JsonNumber(r.body, "expires_in");
            g_accessExpires = SteadyMs() + (ttl > 0 ? ttl : 3600) * 1000;
        }
        LOG_INFO("LuaFlipperUI: signed in to lua.tools through the browser");
        return page("Signed in",
                    "You can close this tab and go back to Steam.");
    }

    /** Redeem a Discord bot code into a session, and keep the durable half. */
    std::string JsonFixesLogin(const std::string& fullPath) {
        std::string code = QueryParam(fullPath, "code");
        for (char& c : code) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (code.size() < 4 || code.size() > 12 ||
            code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
            return "{\"error\":\"That does not look like a bot code. Run /login with "
                   "their Discord bot and it replies with a short code.\"}";

        // Step 1: the code becomes a magic-link token hash.
        auto r = PostJson("https://lua.tools/api/auth/code/redeem",
                          "{\"code\":\"" + JsonEscape(code) + "\"}", L"");
        if (!r.ok) return "{\"error\":\"lua.tools is unreachable\"}";
        if (r.status == 404 || r.status == 400)
            return "{\"error\":\"That code is not valid.\"}";
        if (r.status == 410)
            return "{\"error\":\"That code has expired. They last five minutes; "
                   "run /login again.\"}";
        if (r.status != 200)
            return "{\"error\":\"lua.tools refused the code (HTTP " +
                   std::to_string(r.status) + ")\"}";

        const std::string hash = JsonString(r.body, "token");
        if (hash.empty()) return "{\"error\":\"no token in the reply\"}";

        // Step 2: the hash becomes a real session.
        const std::string anon(kSupabaseAnon);
        auto v = PostJson(std::string(kSupabase) + "/auth/v1/verify",
                          "{\"type\":\"magiclink\",\"token_hash\":\"" +
                          JsonEscape(hash) + "\"}",
                          std::wstring(L"apikey: ") +
                          std::wstring(anon.begin(), anon.end()) + L"\r\n");
        if (!v.ok || v.status != 200)
            return "{\"error\":\"The code was accepted but the session was refused.\"}";

        const std::string refresh = JsonString(v.body, "refresh_token");
        const std::string access  = JsonString(v.body, "access_token");
        if (refresh.empty()) return "{\"error\":\"no refresh token in the session\"}";

        std::string werr;
        if (!WriteConfigKey("fixes", "refresh_token", TomlString(refresh), werr))
            return "{\"error\":\"Signed in, but the session could not be saved: " +
                   JsonEscape(werr) + "\"}";

        {
            std::lock_guard<std::mutex> lock(g_sessionLock);
            g_accessToken = access;
            const long long ttl = JsonNumber(v.body, "expires_in");
            g_accessExpires = SteadyMs() + (ttl > 0 ? ttl : 3600) * 1000;
        }

        const std::string email = JsonString(v.body, "email");
        LOG_INFO("LuaFlipperUI: signed in to lua.tools");
        return "{\"ok\":true,\"email\":\"" + JsonEscape(email) + "\"}";
    }

    std::string JsonFixesLogout() {
        {
            std::lock_guard<std::mutex> lock(g_sessionLock);
            g_accessToken.clear();
            g_accessExpires = 0;
        }
        std::string err;
        WriteConfigKey("fixes", "refresh_token", TomlString(""), err);
        LOG_INFO("LuaFlipperUI: signed out of lua.tools");
        return "{\"ok\":true}";
    }

    /** Whether there is a session, and what it is worth today. */
    std::string JsonFixesAccount() {
        if (Config::GetFixesRefreshToken().empty() && Config::GetFixesToken().empty())
            return "{\"signedIn\":false}";

        std::string err;
        const std::string access = FixesAccessToken(err);
        if (access.empty())
            return "{\"signedIn\":false,\"error\":\"" + JsonEscape(err) + "\"}";

        // Supporter status is the cheapest call that proves the token works.
        const std::wstring headers =
            std::wstring(L"Authorization: Bearer ") +
            std::wstring(access.begin(), access.end()) + L"\r\n" + kBrowserUA;
        auto r = SFPlatform::Http::Execute(
            L"GET", "https://lua.tools/api/me/supporter-status", nullptr, 0,
            headers.c_str(), 5000, 5000, 10000, 15000, 64u * 1024u);
        if (!r.ok || r.status != 200)
            return "{\"signedIn\":false,\"error\":\"the session was refused; "
                   "sign in again\"}";

        return std::string("{\"signedIn\":true,\"supporter\":") +
               (r.body.find("\"isSupporter\":true") != std::string::npos
                ? "true" : "false") + ",\"limit\":25}";
    }

    /* -------------------------------------------------------------- fixes --- */

    // LuaTools' fix catalog. Reading it needs no account: /denuvo/listings and
    // /denuvo/fixes both answer unauthenticated, and only /denuvo/download is
    // behind a bearer token. Verified against the live service rather than
    // assumed, because the whole page is shaped by which half is free.
    constexpr const char* kFixBase = "https://lua.tools/api/denuvo";

    /**
     * The games this install has a manifest for that the fix catalog carries.
     *
     * Intersected here rather than in the page: the catalog is 1700-odd entries
     * and half a megabyte, and shipping all of it to a UI that will draw twenty
     * tiles would spend the whole payload on rows nobody sees. The count of
     * what was searched is kept, so the page can say what it looked through.
     *
     * Also carries this install's own diagnosis per app, the keyless appid
     * count, because "this manifest registers ownership it cannot decrypt" and
     * "there is a fix published for this game" are the two things worth knowing
     * before opening a game, and one request should answer both.
     */
    std::string JsonFixCatalog() {
        auto r = SFPlatform::Http::Execute(L"GET",
            (std::string(kFixBase) + "/listings").c_str(), nullptr, 0, nullptr,
            5000, 5000, 10000, 20000, 4u * 1024u * 1024u);
        if (!r.ok || r.status != 200)
            return "{\"error\":\"The fix catalog is unreachable\"}";

        // What this install has, and what is wrong with it.
        struct Local { size_t keyless = 0, keyed = 0; };
        std::map<std::string, Local> mine;
        for (const auto& p : LuaFiles()) {
            const std::string stem = p.stem().string();
            if (stem.find_first_not_of("0123456789") != std::string::npos) continue;
            const std::string body = ReadWholeFile(p);
            Local l;
            for (size_t i = body.find("addappid("); i != std::string::npos;
                 i = body.find("addappid(", i + 1)) {
                const size_t close = body.find(')', i);
                if (close == std::string::npos) break;
                // Three arguments means the third is a decryption key; one is
                // ownership only. Same split JsonManifests draws.
                (body.substr(i, close - i).find(',') == std::string::npos
                     ? l.keyless : l.keyed)++;
            }
            mine[stem] = l;
        }

        std::string j = "{\"games\":[";
        bool first = true;
        size_t scanned = 0;
        for (size_t at = r.body.find("\"appid\""); at != std::string::npos;
             at = r.body.find("\"appid\"", at + 1)) {
            scanned++;
            const std::string appid = JsonString(r.body, "appid", at);
            auto it = mine.find(appid);
            if (it == mine.end()) continue;

            j += first ? "" : ",";
            first = false;
            j += "{\"appid\":\"" + JsonEscape(appid) + "\"";
            j += ",\"name\":\"" + JsonEscape(JsonString(r.body, "name", at)) + "\"";
            j += ",\"fixes\":" + std::to_string(JsonNumber(r.body, "fixCount", at));
            j += ",\"keyless\":" + std::to_string(it->second.keyless);
            j += ",\"keyed\":" + std::to_string(it->second.keyed) + "}";
        }
        j += "],\"scanned\":" + std::to_string(scanned);
        j += ",\"installed\":" + std::to_string(mine.size()) + "}";
        return j;
    }

    // Every fix published for one app, passed through as the service sends it:
    // the page draws the titles, tags and descriptions verbatim, and rewriting
    // them here would mean this module deciding what a fix says about itself.
    std::string JsonFixList(const std::string& appId) {
        auto r = SFPlatform::Http::Execute(L"GET",
            (std::string(kFixBase) + "/fixes?appid=" + appId).c_str(), nullptr, 0,
            nullptr, 5000, 5000, 10000, 20000, 2u * 1024u * 1024u);
        if (r.ok && r.status == 404) return "{\"fixes\":[]}";
        if (!r.ok || r.status != 200)
            return "{\"error\":\"No fix list for this app\"}";
        return r.body;
    }

    /**
     * Fetch one fix archive to disk.
     *
     * Downloaded, not applied. Fixes do not share a way in: most want the
     * archive extracted over the game, some ship an installer to run, some need
     * a file put somewhere particular, and the difference lives in prose that
     * differs per release. Guessing wrong breaks an installed game, so what to
     * do with the archive stays with whoever read the instructions.
     *
     * `appId` only buys the reply the game's folder, so the page can offer to
     * do the extraction for the fixes where that is the whole of it. Nothing is
     * written outside the fixes directory here.
     *
     * This is the one endpoint in the catalog that needs an account. Without a
     * token the service answers 401, which is reported as the missing setting
     * it is rather than as a network failure.
     */
    std::string JsonFixDownload(const std::string& fixId, const std::string& name,
                                const std::string& slot,
                                const std::string& appId) {
        if (fixId.empty()) return "{\"error\":\"no fix id\"}";
        const std::string want = (slot == "manifest") ? "manifest" : "fix";

        std::string err;
        const std::string access = FixesAccessToken(err);
        if (access.empty()) {
            // needsLogin is what the page acts on; the sentence is only the
            // fallback for anyone reading the API directly, so it does not
            // describe a button they cannot see.
            return "{\"error\":\"This fix is hosted by lua.tools, and only the "
                   "catalog is free. Sign in with Discord to download it.\","
                   "\"needsLogin\":true}";
        }

        const std::wstring auth =
            std::wstring(L"Authorization: Bearer ") +
            std::wstring(access.begin(), access.end()) + L"\r\n" + kBrowserUA;

        /*
         * Two requests, not one.
         *
         * The endpoint does not serve the archive; it answers {"url": "..."}
         * with a short-lived signed R2 link, and the file comes from there with
         * no auth header of its own. Writing the first reply straight to disk
         * produced a zip containing a line of JSON, which is what this used to
         * do. The slot matters too: "fix" is the game archive and "manifest" is
         * the version-pinned lua, and asking for neither got the default.
         */
        const std::string api = std::string(kFixBase) + "/download?fix=" + fixId +
                                "&slot=" + want;
        auto r = SFPlatform::Http::Execute(L"GET", api.c_str(), nullptr, 0,
                                           auth.c_str(), 5000, 5000, 10000, 30000,
                                           256u * 1024u);
        if (r.ok && (r.status == 401 || r.status == 403))
            return "{\"error\":\"lua.tools refused the session. Sign in again.\","
                   "\"needsLogin\":true}";
        if (r.ok && r.status == 429)
            return "{\"error\":\"The daily download limit for this account is "
                   "spent. It is 25 a day, shared with manifest downloads.\"}";
        if (!r.ok || r.status != 200)
            return "{\"error\":\"The fix could not be requested\"}";

        const std::string signed_ = JsonString(r.body, "url");
        if (signed_.empty())
            return "{\"error\":\"No download link came back\"}";

        // The link carries its own credentials, so this one goes out bare.
        auto f = SFPlatform::Http::Execute(L"GET", signed_.c_str(), nullptr, 0,
                                           kBrowserUA, 5000, 5000, 10000, 180000,
                                           512u * 1024u * 1024u);
        if (!f.ok || f.status != 200 || f.body.empty())
            return "{\"error\":\"The signed link did not serve the file\"}";

        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::path(g_steamPath) / "steamflipper" / "fixes";
        std::filesystem::create_directories(dir, ec);

        std::string file = name.empty() ? (fixId + ".zip") : name;
        if (file.find('/') != std::string::npos ||
            file.find('\\') != std::string::npos || file == "..")
            file = fixId + ".zip";

        const std::filesystem::path out = dir / file;
        std::ofstream o(out, std::ios::binary);
        if (!o) return "{\"error\":\"Could not write to " +
                       JsonEscape(dir.string()) + "\"}";
        o.write(f.body.data(), static_cast<std::streamsize>(f.body.size()));
        o.close();

        LOG_INFO("LuaFlipperUI: fix {} saved ({} bytes)", file, f.body.size());

        // Only for the fix slot. A manifest is a lua for the loader, and there
        // is no sense in offering to drop one into a game folder.
        std::string where;
        if (want == "fix" && !appId.empty())
            where = LuaFlipperDownload::GameDir(appId, g_steamPath);

        return "{\"ok\":true,\"path\":\"" + JsonEscape(out.string()) +
               "\",\"bytes\":" + std::to_string(f.body.size()) +
               ",\"file\":\"" + JsonEscape(file) +
               "\",\"gameDir\":\"" + JsonEscape(where) + "\"}";
    }

    /**
     * Extract an already-downloaded fix over the game, on request.
     *
     * Deliberately its own endpoint rather than a step of the download. It does
     * one thing -- the "extract to the game folder" line that most fixes open
     * with -- and it is asked for by someone who has the rest of the
     * instructions in front of them.
     *
     * `name` is a filename in the fixes directory, never a path: it comes back
     * from the download that wrote it, and is re-checked here because the
     * server will answer anyone who can reach the port.
     */
    std::string JsonFixApply(const std::string& appId, const std::string& name) {
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name.find("..") != std::string::npos)
            return "{\"error\":\"bad archive name\"}";

        const std::filesystem::path zip =
            std::filesystem::path(g_steamPath) / "steamflipper" / "fixes" / name;
        std::error_code ec;
        if (!std::filesystem::exists(zip, ec))
            return "{\"error\":\"That archive is not in the fixes folder any "
                   "more. Download it again.\"}";

        const std::string dir = LuaFlipperDownload::GameDir(appId, g_steamPath);
        if (dir.empty())
            return "{\"error\":\"Steam has no install folder for this app.\"}";

        const std::string res = LuaFlipperDownload::Apply(zip.string(), dir);
        LOG_INFO("LuaFlipperUI: applied {} to {}", name, dir);
        return res;
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
    /**
     * Put a removed manifest back.
     *
     * Only reachable while the tombstone exists, which is until the next Steam
     * start. Steam has already forgotten the ownership by then and the file is
     * swept, so there is nothing to restore and nothing that claims otherwise.
     */
    std::string JsonRestore(const std::string& appId) {
        std::error_code ec;
        for (const auto& p : RemovedFiles()) {
            if (std::filesystem::path(p.stem()).stem().string() != appId) continue;
            std::filesystem::path back = p;
            back.replace_extension();          // drop ".removed"
            if (std::filesystem::exists(back, ec)) {
                return "{\"error\":\"" + JsonEscape(back.filename().string()) +
                       " already exists; the manifest was re-added since it was "
                       "removed\"}";
            }
            std::filesystem::rename(p, back, ec);
            if (ec) {
                return "{\"error\":\"could not restore " +
                       JsonEscape(p.filename().string()) + ": " +
                       JsonEscape(ec.message()) + "\"}";
            }
            LOG_INFO("LuaFlipperUI: restored {}", back.filename().string());
            return "{\"ok\":true,\"file\":\"" +
                   JsonEscape(back.filename().string()) + "\"}";
        }
        return "{\"error\":\"nothing removed for that app id\"}";
    }

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
        j += ",\"version\":\"" + JsonEscape(s.version) + "\"";
        j += ",\"remote_version\":\"" + JsonEscape(s.remoteVersion) + "\"";
        j += ",\"behind\":" + std::string(s.behind ? "true" : "false");
        // relation is "" when ancestry could not be settled (no [update].repo,
        // or the fetch failed), in which case the page should not claim a
        // direction it does not have.
        j += ",\"relation\":\"" + JsonEscape(s.relation) + "\"";
        j += ",\"ahead\":" + std::to_string(s.ahead);
        j += ",\"behind_by\":" + std::to_string(s.behindBy) + "}";
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
    std::filesystem::path StateDir() {
        return std::filesystem::path(g_steamPath) / "steamflipper";
    }

    /**
     * Pull, and then either finish the job or say how to.
     *
     * `automatic` hands the rest to a detached helper: it closes Steam, builds,
     * installs and starts Steam again. That sequence cannot happen in here --
     * the installer replaces the module that is running this code, so Steam has
     * to be gone first, which means whatever does it has to outlive the process
     * that asked.
     *
     * Which is also why the reply is thin. Once the helper is up, this process
     * has seconds to live and no way to report what happens after; the helper
     * writes <Steam>/steamflipper/update-status and the next session reads it.
     */
    std::string JsonSourceApply(bool automatic) {
        const AppUpdater::PullResult p = AppUpdater::PullSource();
        if (!p.ok) {
            return "{\"error\":\"" + JsonEscape(p.error) + "\",\"status\":\"" +
                   JsonEscape(p.status) + "\"}";
        }

        std::string j = "{\"ok\":true";
        j += ",\"pulled\":\"" + JsonEscape(p.sha) + "\"";
        j += ",\"status\":\"" + JsonEscape(p.status) + "\"";

        if (automatic) {
            if (AppUpdater::LaunchAutoUpdate(StateDir().string()))
                return j + ",\"automatic\":true}";
            // Fall through to the manual instructions rather than reporting a
            // failure: the pull did happen, and the only thing lost is who runs
            // the installer.
            j += ",\"automatic\":false";
            j += ",\"note\":\"the helper could not be started\"";
        }

        j += ",\"command\":\"" +
             JsonEscape("cd " + p.repo + " && ./tools/install_linux.sh") + "\"}";
        return j;
    }

    // The previous helper run, for the page to report once Steam is back.
    std::string JsonLastUpdate() {
        const AppUpdater::LastUpdate u = AppUpdater::ReadLastUpdate(StateDir().string());
        if (u.state.empty()) return "{\"state\":\"\"}";
        return "{\"state\":\"" + JsonEscape(u.state) +
               "\",\"when\":\"" + JsonEscape(u.when) +
               "\",\"message\":\"" + JsonEscape(u.message) + "\"}";
    }

    /**
     * Whether the store view is being shown as the Unlocker tab.
     *
     * The client window and the store are separate CEF targets that cannot see
     * each other's globals, so the flag lives here, where both already talk. The
     * client sets it when the tab opens and clears it on the way out; the store
     * script reads it to decide whether to present prices as free and send Add
     * to Cart to LUAFlipper instead.
     *
     * Held as a lease rather than a plain flag, because the cost of the two
     * failure directions is not symmetric. Expiring early only drops the free
     * pricing off our own tab; staying on by mistake rewrites the real Store
     * tab, where the user may be about to spend money. So the client has to keep
     * saying it is still there, and anything that stops the tab from saying so,
     * a crashed page script, a missed close, a navigation nobody handled, ends
     * with the store back to normal on its own.
     *
     * Deliberately not persisted. If the client dies with the lease held, the
     * next start should show an ordinary store, not a silently rewritten one.
     */
    constexpr auto kUnlockerLease = std::chrono::seconds(6);
    std::atomic<long long> g_unlockerUntil{0};   // steady ms; 0 is off

    long long SteadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool UnlockerModeOn() {
        return g_unlockerUntil.load(std::memory_order_acquire) > SteadyMs();
    }

    std::string JsonUnlockerMode(const std::string& fullPath) {
        const std::string set = QueryParam(fullPath, "set");
        if (!set.empty()) {
            const bool on = (set == "1" || set == "true");
            g_unlockerUntil.store(
                on ? SteadyMs() + std::chrono::duration_cast<std::chrono::milliseconds>(
                                      kUnlockerLease).count()
                   : 0,
                std::memory_order_release);
            LOG_DEBUG("LuaFlipperUI: unlocker mode {}", on ? "on" : "off");
        }
        const long long until = g_unlockerUntil.load(std::memory_order_acquire);
        return std::string("{\"on\":") +
               (until > SteadyMs() ? "true" : "false") + "}";
    }


    /* -------------------------------------------------------- store bridge --- */

    // Defined below, once every endpoint it dispatches to exists.
    std::string RouteApi(const std::string& fullPath);

    // Page-side names the bridge script talks to. Requests go out through the
    // binding, replies and mode changes come back as evaluated statements.
    constexpr const char* kBridgeName = "__luaflipperBridge";

    /**
     * Whether a store page may ask for this path.
     *
     * The binding is reachable by any script running in the store target, ours
     * or Valve's or whatever a compromised store page might carry, so it is not
     * a general door into the API. Only the two calls the store integration
     * actually makes are allowed through; the mode is pushed rather than
     * requested, so it is not on the list either.
     */
    bool BridgeAllows(const std::string& path) {
        const std::string p = path.substr(0, path.find('?'));
        return p == "/api/sources" || p == "/api/install";
    }

    /**
     * Serve one store target for as long as it lives.
     *
     * Steam's store pages carry a CSP whose connect-src permits Valve's own
     * hosts and Steam's local helper port, and nothing else, so a script in that
     * page cannot fetch the module the way the client-window UI does. A CDP
     * binding is not a network request and so is not subject to that, which
     * makes it the channel that works without weakening the page: no CSP bypass,
     * and no reachable surface beyond BridgeAllows.
     *
     * The connection has to stay open because it *is* the channel, unlike the
     * main window where injection is one-shot. Losing it ends the thread, and
     * the injector loop notices and starts a new one.
     */
    void StoreBridgeThread(std::string wsUrl, std::string script,
                           std::shared_ptr<std::atomic<bool>> alive) {
        WebSocket s;
        if (!s.Open(wsUrl)) { alive->store(false); return; }
        // Short enough that a mode change reaches the page promptly, since the
        // loop can only push it between reads.
        s.SetRecvTimeout(1);

        s.Call("Page.enable", "{}");
        s.Call("Runtime.enable", "{}");
        s.Call("Runtime.addBinding",
               std::string("{\"name\":\"") + kBridgeName + "\"}");
        const std::string src = JsonEscape(script);
        s.Call("Page.addScriptToEvaluateOnNewDocument",
               "{\"source\":\"" + src + "\"}");
        s.Call("Runtime.evaluate", "{\"expression\":\"" + src + "\"}");

        bool pushed = false;
        bool needPush = true;      // the freshly injected script knows nothing yet

        while (!s.closed) {
            const std::string msg = s.Recv();

            if (!msg.empty() && msg.find("\"Runtime.bindingCalled\"") != std::string::npos) {
                const std::string payload = JsonString(msg, "payload");
                const long long id = JsonNumber(payload, "id");
                const std::string path = JsonString(payload, "path");
                std::string body = "{\"ok\":false,\"error\":\"refused\"}";
                if (id >= 0 && BridgeAllows(path)) body = RouteApi(path);
                // Serving happens on this thread on purpose: an install downloads
                // and unpacks through libcurl, which is not safe to enter from
                // several threads at once. One store view, one request at a time.
                s.Post("Runtime.evaluate",
                       "{\"expression\":\"" +
                       JsonEscape("window.__luaflipperBridgeReply&&window."
                                  "__luaflipperBridgeReply(" + std::to_string(id) +
                                  "," + body + ")") + "\"}");
            }

            // A new document starts with none of our state, so re-send it.
            if (!msg.empty() &&
                (msg.find("Runtime.executionContextsCleared") != std::string::npos ||
                 msg.find("\"Page.frameNavigated\"") != std::string::npos))
                needPush = true;

            const bool on = UnlockerModeOn();
            if (needPush || on != pushed) {
                s.Post("Runtime.evaluate",
                       "{\"expression\":\"" +
                       JsonEscape(std::string("window.__luaflipperMode=") +
                                  (on ? "true" : "false") +
                                  ";window.__luaflipperSetMode&&window.__luaflipperSetMode()") +
                       "\"}");
                pushed = on;
                needPush = false;
            }
        }

        s.Close();
        alive->store(false);
    }

    /**
     * One request at a time, whichever thread asked.
     *
     * The loopback server was written serial on purpose: several endpoints
     * download through libcurl, which the steam-runtime hands us without a
     * thread-safe implicit curl_global_init, and entering it from two threads
     * at once took the whole client down twice while this was being built. The
     * store bridge added a second caller, one per store view, so the guarantee
     * that used to come from having a single thread now has to be stated.
     */
    std::mutex g_apiLock;

    std::string RouteApi(const std::string& fullPath) {
        std::lock_guard<std::mutex> serialize(g_apiLock);
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
        if (path == "/api/unlocker/mode") return JsonUnlockerMode(fullPath);

        // Fixes. The catalog and the per-app list are free; the download is the
        // one call that needs the user's own lua.tools token.
        if (path == "/api/navmenu/show") return JsonNavMenuShow(fullPath);
        if (path == "/api/navmenu/hide") return JsonNavMenuHide(fullPath);
        if (path == "/api/navmenu/keep") return JsonNavMenuKeep();
        if (path == "/api/navmenu/close") return JsonNavMenuClose();
        if (path == "/api/navmenu/pick") return JsonNavMenuPick(fullPath);

        if (path == "/api/hubcap/stats") return JsonHubcapStats();
        if (path == "/api/hubcap/save")  return JsonHubcapSave(fullPath);
        if (path == "/api/sources/order") return JsonSourcesSave(fullPath);

        if (path == "/api/fixes/signin")   return JsonFixesSignIn();
        if (path == "/api/fixes/callback") return JsonFixesCallback(fullPath);
        if (path == "/api/fixes/login")   return JsonFixesLogin(fullPath);
        if (path == "/api/fixes/logout")  return JsonFixesLogout();
        if (path == "/api/fixes/account") return JsonFixesAccount();

        if (path == "/api/fixes/catalog") return JsonFixCatalog();
        if (path == "/api/fixes/list") {
            const std::string id = QueryParam(fullPath, "appid");
            if (id.empty()) return "{\"error\":\"no appid\"}";
            return JsonFixList(id);
        }
        if (path == "/api/fixes/download") {
            return JsonFixDownload(QueryParam(fullPath, "fix"),
                                   QueryParam(fullPath, "name"),
                                   QueryParam(fullPath, "slot"),
                                   QueryParam(fullPath, "appid"));
        }
        if (path == "/api/fixes/apply") {
            return JsonFixApply(QueryParam(fullPath, "appid"),
                                QueryParam(fullPath, "name"));
        }
        if (path == "/api/cloud")        return JsonCloud();
        if (path == "/api/cloud/enable")  return JsonCloudEnable();
        if (path == "/api/cloud/disable") return JsonCloudDisable();
        if (path == "/api/restore") {
            const std::string id = QueryParam(fullPath, "appid");
            if (id.empty()) return "{\"error\":\"no appid\"}";
            return JsonRestore(id);
        }
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
        if (path == "/api/update/changelog") {
            const std::string md = AppUpdater::Changelog();
            if (md.empty())
                return "{\"error\":\"The changelog could not be read from "
                       "GitHub.\"}";
            return "{\"markdown\":\"" + JsonEscape(md) + "\"}";
        }
        if (path == "/api/update/apply")
            return JsonSourceApply(QueryParam(fullPath, "auto") == "1");
        if (path == "/api/update/last") return JsonLastUpdate();
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
                // The OAuth callback is the one route a browser lands on
                // directly rather than fetches, so it answers HTML. Everything
                // else is JSON for the UI.
                const bool html = body.compare(0, 6, "<html>") == 0;
                std::string resp =
                    std::string("HTTP/1.1 ") + (found ? "200 OK" : "404 Not Found") +
                    (html ? "\r\nContent-Type: text/html; charset=utf-8\r\n"
                          : "\r\nContent-Type: application/json\r\n") +
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
        // Store integration. A separate script because it runs in a separate
        // target: the store is ordinary web pages, not the client's React app.
        const std::string store = ReadWholeFile(uiDir / "luaflipper.store.js");
        // The nav menu as a real window. Lives in SharedJSContext because that
        // is where g_PopupManager is; optional, and its absence only costs the
        // popup route.
        const std::string popup = ReadWholeFile(uiDir / "luaflipper.popup.js");
        if (js.empty()) {
            LOG_ERROR("LuaFlipperUI: no UI assets at {}", uiDir.string());
            return;
        }

        std::string lastTarget;
        // The SharedJSContext the popup helper was last injected into. That
        // context is recreated on its own schedule, and a new one arrives
        // without the helper.
        std::string lastShared;
        // Store targets we are bridging, and whether that bridge is still up.
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> storeTargets;
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

            // Store views come and go as the user navigates, so this runs every
            // pass rather than once. Each gets its own bridge thread, which owns
            // the connection for as long as the view lives; targets are tracked
            // by debugger URL, which carries the target id, so a view that is
            // torn down and rebuilt is picked up again as a new one.
            if (!store.empty()) {
                const std::vector<std::string> live = StoreTargets(list);
                for (const std::string& t : live) {
                    auto held = storeTargets.find(t);
                    if (held != storeTargets.end()) {
                        if (held->second->load()) continue;      // still serving
                        storeTargets.erase(held);                // died, take it again
                    }
                    auto alive = std::make_shared<std::atomic<bool>>(true);
                    storeTargets[t] = alive;
                    std::thread(StoreBridgeThread, t, store, alive).detach();
                    LOG_INFO("LuaFlipperUI: store bridge attached to {}", t);
                }
                // Forget the ones that are gone, or the map grows for as long as
                // Steam runs.
                for (auto it = storeTargets.begin(); it != storeTargets.end();)
                    it = (std::find(live.begin(), live.end(), it->first) == live.end())
                             ? storeTargets.erase(it) : std::next(it);
            }

            // SharedJSContext holds Steam's popup manager and nothing of ours,
            // so this is injected separately from the client window's UI and
            // re-checked each pass: that context is recreated on its own
            // schedule and takes the helper with it when it goes.
            if (!popup.empty()) {
                const size_t sj = list.find("\"SharedJSContext\"");
                if (sj != std::string::npos) {
                    const std::string sjws = JsonString(list, "webSocketDebuggerUrl", sj);
                    if (!sjws.empty() && sjws != lastShared) {
                        if (InjectInto(sjws, popup)) {
                            lastShared = sjws;
                            LOG_INFO("LuaFlipperUI: nav popup helper injected");
                        }
                    }
                }
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
    g_steamPath = steamInstallPath ? steamInstallPath : "";

    // Ahead of every gate below, because this is disk hygiene rather than UI: a
    // tombstone from a previous session is a removal the user did not undo, and
    // its undo window closed when Steam restarted. Behind the gates it would
    // survive forever on an install that later turned the UI off.
    SweepRemoved();

    // The downloader offers lua.tools' proxied sources only while a session
    // exists, and the session lives here, so it is handed a way to ask.
    LuaFlipperDownload::SetTokenProvider([]() {
        std::string err;
        return FixesAccessToken(err);
    });

    if (!Config::GetUiEnabled()) {
        LOG_INFO("LuaFlipperUI: [ui].enabled is false, client UI disabled");
        return;
    }

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
    std::thread(SteamUrlWatcher).detach();
#else
    (void)steamInstallPath;
#endif
}

} // namespace LuaFlipperUI
