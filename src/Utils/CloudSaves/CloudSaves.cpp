#include "CloudSaves.h"

#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include "cloud_messages.pb.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace CloudSaves {

// Defined below; Initialize() and SyncAppSet() both seed from it.
static std::vector<uint32_t> ManifestAppIds();
namespace {

    std::mutex        g_mutex;
    std::atomic<bool> g_active{false};

    std::unordered_set<uint32_t> g_appIds;

#if defined(__linux__)

    namespace fs = std::filesystem;

    // EResult values the client understands (Steam/Enums.h).
    constexpr int32_t kEResultOK           = 1;
    constexpr int32_t kEResultFail         = 2;
    constexpr int32_t kEResultFileNotFound = 9;

    // EHTTPMethod: the block request tells the client which verb to use.
    constexpr int32_t kHttpMethodPut = 4;

    // Reported through ClientGetAppQuotaUsage. Nothing enforces a quota here,
    // but the client refuses to start a sync against a zeroed one.
    constexpr uint32_t kQuotaMaxFiles = 10000;
    constexpr uint64_t kQuotaMaxBytes = 1024ull * 1024ull * 1024ull;

    // A save that does not fit in a 32-bit process' address space twice over is
    // not a save. Bounds the loopback endpoint's per-request allocation.
    constexpr uint64_t kMaxBlobBytes = 128ull * 1024ull * 1024ull;

    // Request line plus headers. Steam sends a handful; this is pure headroom.
    constexpr size_t kMaxHeaderBytes = 16u * 1024u;

    /* ---------------------------------------------------------------- store --- */

    struct FileRecord {
        uint64_t    timestamp    = 0;
        uint32_t    size         = 0;
        uint32_t    platforms    = 0xFFFFFFFFu;
        uint32_t    persistState = 0;   // ECloudStoragePersistState: 0 = Persisted
        std::string sha;                // raw SHA1 bytes, exactly as the client sent them
        std::string root;               // the "%Token%/" prefix stripped off the name
    };

    struct AppState {
        uint64_t                                    changeNumber  = 0;
        uint64_t                                    activeBatchId = 0;
        std::unordered_map<std::string, FileRecord> files;   // key: clean relative name
        bool                                        loaded = false;
    };

    std::string                             g_root;      // <steam>/steamflipper/cloudsaves
    std::unordered_map<uint64_t, AppState>  g_apps;      // key: account<<32 | app
    std::unordered_map<std::string, FileRecord> g_pending;  // uploads announced but not committed
    std::atomic<uint64_t>                   g_nextBatchId{1};

    uint64_t AppKey(uint32_t accountId, uint32_t appId) {
        return (static_cast<uint64_t>(accountId) << 32) | appId;
    }

    fs::path AccountDir(uint32_t accountId) {
        return fs::path(g_root) / std::to_string(accountId);
    }

    fs::path AppDir(uint32_t accountId, uint32_t appId) {
        return AccountDir(accountId) / std::to_string(appId);
    }

    // Beside the app subtree rather than inside it, so no save file can ever
    // collide with (or overwrite) the record of what the store holds.
    fs::path ManifestPath(uint32_t accountId, uint32_t appId) {
        return AccountDir(accountId) / (std::to_string(appId) + ".manifest");
    }

    /* ---------------------------------------------------------- path safety --- */

    // A name off the wire may only ever land inside the app's own subtree. Same
    // rule LuaFlipperDownload applies to zip entries, plus a control-character
    // ban: the manifest is line based, so a newline in a name would forge a
    // record, and a tab would forge a field.
    bool IsSafeRelativeName(const std::string& n) {
        if (n.empty() || n.size() > 1024) return false;
        if (n.front() == '/') return false;
        if (n.back() == '/') return false;
        if (n.find("..") != std::string::npos) return false;
        for (unsigned char c : n)
            if (c < 0x20 || c == 0x7F) return false;
        return true;
    }

    // Steam prefixes a cloud filename with the storage root it came from, e.g.
    // "%WinMyDocuments%/My Games/save.dat". The token is not part of the file's
    // identity, so it is split off for storage and kept verbatim (separator and
    // all) so the changelist can hand the exact same prefix back.
    void SplitRoot(const std::string& filename, std::string& root, std::string& clean) {
        root.clear();
        clean = filename;
        if (filename.size() < 2 || filename[0] != '%') return;

        const size_t end = filename.find('%', 1);
        if (end == std::string::npos || end + 1 >= filename.size()) return;

        size_t at = end + 1;
        while (at < filename.size() && filename[at] == '/') at++;
        root  = filename.substr(0, at);
        clean = filename.substr(at);
    }

    /* --------------------------------------------------------------- atomic --- */

    // Temp file inside the target directory so the rename is same-filesystem and
    // therefore atomic: a crash mid-write cannot leave a half-written save under
    // the name of the good one.
    bool WriteAtomic(const fs::path& target, const char* data, size_t size) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (!fs::is_directory(target.parent_path(), ec)) return false;

        std::string tmp = target.string() + ".sfXXXXXX";
        const int fd = ::mkstemp(tmp.data());
        if (fd < 0) {
            LOG_WARN("CloudSaves: mkstemp {} failed: {}", tmp, std::strerror(errno));
            return false;
        }
        ::fchmod(fd, 0644);   // mkstemp opens 0600; a save file is not a secret

        bool ok = true;
        for (size_t off = 0; off < size; ) {
            const ssize_t w = ::write(fd, data + off, size - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = false; break; }
            off += static_cast<size_t>(w);
        }
        // Flush before the rename, or a crash can publish the name with no bytes
        // behind it.
        if (ok && ::fsync(fd) != 0) ok = false;
        ::close(fd);

        if (ok && ::rename(tmp.c_str(), target.c_str()) != 0) {
            LOG_WARN("CloudSaves: rename to {} failed: {}", target.string(), std::strerror(errno));
            ok = false;
        }
        if (!ok) ::unlink(tmp.c_str());
        return ok;
    }

    bool ReadWholeFile(const fs::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    /* -------------------------------------------------------------- manifest --- */

    std::string HexEncode(const std::string& raw) {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(raw.size() * 2);
        for (unsigned char c : raw) {
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
        return out;
    }

    bool HexDecode(const std::string& hex, std::string& out) {
        if (hex.size() % 2) return false;
        out.clear();
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            int v = 0;
            for (int k = 0; k < 2; k++) {
                const char c = hex[i + k];
                v <<= 4;
                if (c >= '0' && c <= '9')      v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else return false;
            }
            out += static_cast<char>(v);
        }
        return true;
    }

    // One record per line, tab separated, name last. Tabs and newlines are
    // rejected by IsSafeRelativeName, so a name can never split a record.
    //   cn <changeNumber>
    //   size <TAB> mtime <TAB> persist <TAB> platforms <TAB> shaHex <TAB> root <TAB> name
    void LoadManifest(uint32_t accountId, uint32_t appId, AppState& st) {
        std::string text;
        if (!ReadWholeFile(ManifestPath(accountId, appId), text)) return;

        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("cn ", 0) == 0) {
                st.changeNumber = std::strtoull(line.c_str() + 3, nullptr, 10);
                continue;
            }

            std::string field[6];
            size_t at = 0;
            bool   ok = true;
            for (int i = 0; i < 6 && ok; i++) {
                const size_t tab = line.find('\t', at);
                if (tab == std::string::npos) { ok = false; break; }
                field[i] = line.substr(at, tab - at);
                at = tab + 1;
            }
            if (!ok || at >= line.size()) continue;

            const std::string name = line.substr(at);
            if (!IsSafeRelativeName(name)) continue;   // a hand-edited manifest is still input

            FileRecord rec;
            rec.size         = static_cast<uint32_t>(std::strtoul(field[0].c_str(), nullptr, 10));
            rec.timestamp    = std::strtoull(field[1].c_str(), nullptr, 10);
            rec.persistState = static_cast<uint32_t>(std::strtoul(field[2].c_str(), nullptr, 10));
            rec.platforms    = static_cast<uint32_t>(std::strtoul(field[3].c_str(), nullptr, 10));
            if (field[4] != "-" && !HexDecode(field[4], rec.sha)) continue;
            if (field[5] != "-") rec.root = field[5];
            st.files[name] = std::move(rec);
        }
    }

    void SaveManifest(uint32_t accountId, uint32_t appId, const AppState& st) {
        std::string text = "cn " + std::to_string(st.changeNumber) + "\n";
        for (const auto& [name, rec] : st.files) {
            text += std::to_string(rec.size);         text += '\t';
            text += std::to_string(rec.timestamp);    text += '\t';
            text += std::to_string(rec.persistState); text += '\t';
            text += std::to_string(rec.platforms);    text += '\t';
            text += rec.sha.empty()  ? "-" : HexEncode(rec.sha);  text += '\t';
            text += rec.root.empty() ? "-" : rec.root;            text += '\t';
            text += name;
            text += '\n';
        }
        if (!WriteAtomic(ManifestPath(accountId, appId), text.data(), text.size()))
            LOG_WARN("CloudSaves: could not persist manifest for account={} app={}", accountId, appId);
    }

    // Caller holds g_mutex.
    AppState& GetState(uint32_t accountId, uint32_t appId) {
        AppState& st = g_apps[AppKey(accountId, appId)];
        if (!st.loaded) {
            LoadManifest(accountId, appId, st);
            st.loaded = true;
        }
        return st;
    }

    std::string PendingKey(uint32_t accountId, uint32_t appId, const std::string& name) {
        return std::to_string(accountId) + "/" + std::to_string(appId) + "/" + name;
    }

    /* ------------------------------------------------------------------ url --- */

    bool IsUnreserved(unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    }

    // Path separators stay literal so the target still reads as a path; every
    // other byte is escaped, which covers spaces and non-ASCII save names.
    std::string UrlEncodePath(const std::string& s) {
        static const char* kHex = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size() + 16);
        for (unsigned char c : s) {
            if (IsUnreserved(c) || c == '/') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += kHex[c >> 4];
                out += kHex[c & 0x0F];
            }
        }
        return out;
    }

    bool UrlDecode(const std::string& s, std::string& out) {
        out.clear();
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] != '%') { out += s[i]; continue; }
            if (i + 2 >= s.size()) return false;
            std::string byte;
            if (!HexDecode(s.substr(i + 1, 2), byte)) return false;
            out += byte;
            i += 2;
        }
        return true;
    }

    /* -------------------------------------------------------- loopback http --- */
    //
    // File bytes never travel inside the RPCs: the reply names an HTTP host and
    // the client transfers against it. So the store needs an endpoint of its
    // own. It binds 127.0.0.1 on an ephemeral port and speaks exactly two
    // routes:
    //   PUT /u/<accountId>/<appId>/<url-encoded name>
    //   GET /d/<accountId>/<appId>/<url-encoded name>
    //
    // Connections are served one at a time on the accept thread. Steam issues
    // one transfer per file and every byte is local, so a queue of two is
    // already unusual; a thread per connection would buy nothing but lifetime
    // hazards inside someone else's process.

    int                   g_listenFd = -1;
    uint16_t              g_port     = 0;
    std::thread           g_httpThread;
    std::atomic<bool>     g_httpRunning{false};

    void SendAll(int fd, const char* data, size_t size) {
        for (size_t off = 0; off < size; ) {
            const ssize_t w = ::send(fd, data + off, size - off, MSG_NOSIGNAL);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return;
            off += static_cast<size_t>(w);
        }
    }

    void RespondStatus(int fd, const char* status) {
        const std::string r = std::string("HTTP/1.1 ") + status +
                              "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        SendAll(fd, r.data(), r.size());
    }

    void RespondBody(int fd, const std::string& body) {
        const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                 "Content-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n";
        SendAll(fd, head.data(), head.size());
        SendAll(fd, body.data(), body.size());
    }

    // "/u/12345/730/saves%2Fslot1.dat" -> kind 'u', account, app, clean name.
    bool ParseTarget(const std::string& target, char& kind,
                     uint32_t& accountId, uint32_t& appId, std::string& name) {
        if (target.size() < 4 || target[0] != '/' || target[2] != '/') return false;
        kind = target[1];
        if (kind != 'u' && kind != 'd') return false;

        const size_t s1 = target.find('/', 3);
        if (s1 == std::string::npos) return false;
        const size_t s2 = target.find('/', s1 + 1);
        if (s2 == std::string::npos || s2 + 1 >= target.size()) return false;

        accountId = static_cast<uint32_t>(std::strtoul(target.substr(3, s1 - 3).c_str(), nullptr, 10));
        appId     = static_cast<uint32_t>(std::strtoul(target.substr(s1 + 1, s2 - s1 - 1).c_str(), nullptr, 10));
        if (accountId == 0 || appId == 0) return false;

        return UrlDecode(target.substr(s2 + 1), name) && IsSafeRelativeName(name);
    }

    // Case-insensitive Content-Length lookup. Absent means zero: Steam omits the
    // header entirely for empty files, and a 0-byte save is a real save.
    uint64_t ContentLength(const std::string& headers, bool& malformed) {
        malformed = false;
        std::string lower = headers;
        for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

        size_t at = lower.find("\r\ncontent-length:");
        if (at == std::string::npos) return 0;
        at += 17;

        const size_t eol = lower.find("\r\n", at);
        if (eol == std::string::npos) { malformed = true; return 0; }

        const std::string value = headers.substr(at, eol - at);
        char* endp = nullptr;
        const unsigned long long v = std::strtoull(value.c_str(), &endp, 10);
        if (endp == value.c_str()) { malformed = true; return 0; }
        return v;
    }

    void HandleConnection(int fd) {
        // A stalled peer must not pin the accept thread forever.
        struct timeval tv { 30, 0 };
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        std::string buf;
        size_t      headerEnd = std::string::npos;
        char        chunk[4096];
        while (buf.size() < kMaxHeaderBytes) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            buf.append(chunk, static_cast<size_t>(n));
            headerEnd = buf.find("\r\n\r\n");
            if (headerEnd != std::string::npos) break;
        }
        if (headerEnd == std::string::npos) { RespondStatus(fd, "400 Bad Request"); return; }

        const size_t sp1 = buf.find(' ');
        const size_t sp2 = sp1 == std::string::npos ? std::string::npos : buf.find(' ', sp1 + 1);
        if (sp2 == std::string::npos || sp2 > headerEnd) { RespondStatus(fd, "400 Bad Request"); return; }

        const std::string method  = buf.substr(0, sp1);
        const std::string target  = buf.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string headers = buf.substr(0, headerEnd + 2);

        char        kind      = 0;
        uint32_t    accountId = 0;
        uint32_t    appId     = 0;
        std::string name;
        if (!ParseTarget(target, kind, accountId, appId, name)) {
            LOG_WARN("CloudSaves: loopback rejected target {}", target);
            RespondStatus(fd, "404 Not Found");
            return;
        }
        const fs::path blob = AppDir(accountId, appId) / name;

        if (method == "GET" && kind == 'd') {
            std::string body;
            if (!ReadWholeFile(blob, body)) { RespondStatus(fd, "404 Not Found"); return; }
            LOG_DEBUG("CloudSaves: served {} bytes of app={} {}", body.size(), appId, name);
            RespondBody(fd, body);
            return;
        }

        if (method != "PUT" && method != "POST") { RespondStatus(fd, "405 Method Not Allowed"); return; }
        if (kind != 'u')                         { RespondStatus(fd, "404 Not Found");          return; }

        bool           malformed = false;
        const uint64_t want      = ContentLength(headers, malformed);
        if (malformed)             { RespondStatus(fd, "400 Bad Request");      return; }
        if (want > kMaxBlobBytes)  { RespondStatus(fd, "413 Payload Too Large"); return; }

        std::string body = buf.substr(headerEnd + 4);
        if (body.size() > want) body.resize(static_cast<size_t>(want));
        while (body.size() < want) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            const size_t take = std::min(static_cast<size_t>(n), static_cast<size_t>(want) - body.size());
            body.append(chunk, take);
        }
        if (body.size() != want) { RespondStatus(fd, "400 Bad Request"); return; }

        if (!WriteAtomic(blob, body.data(), body.size())) {
            RespondStatus(fd, "500 Internal Server Error");
            return;
        }
        LOG_DEBUG("CloudSaves: stored {} bytes for app={} {}", body.size(), appId, name);
        RespondStatus(fd, "200 OK");
    }

    bool StartHttp() {
        g_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (g_listenFd < 0) return false;

        int on = 1;
        ::setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never reachable off this box
        addr.sin_port        = 0;                        // let the kernel pick

        socklen_t len = sizeof(addr);
        if (::bind(g_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::getsockname(g_listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
            ::listen(g_listenFd, 16) != 0) {
            LOG_WARN("CloudSaves: loopback endpoint failed to start: {}", std::strerror(errno));
            ::close(g_listenFd);
            g_listenFd = -1;
            return false;
        }
        g_port = ntohs(addr.sin_port);

        g_httpRunning.store(true, std::memory_order_release);
        g_httpThread = std::thread([] {
            while (g_httpRunning.load(std::memory_order_acquire)) {
                const int fd = ::accept(g_listenFd, nullptr, nullptr);
                if (fd < 0) {
                    // A peer that hung up between connect and accept is routine
                    // and must not take the endpoint down with it.
                    if (errno == EINTR || errno == ECONNABORTED) continue;
                    break;   // listener closed by Shutdown, or unrecoverable
                }
                HandleConnection(fd);
                ::close(fd);
            }
        });
        return true;
    }

    void StopHttp() {
        if (!g_httpRunning.exchange(false)) return;
        if (g_listenFd >= 0) {
            // Break the blocking accept: shutdown first so a thread already
            // parked in accept returns instead of waiting on a closed fd.
            ::shutdown(g_listenFd, SHUT_RDWR);
            ::close(g_listenFd);
            g_listenFd = -1;
        }
        if (g_httpThread.joinable()) g_httpThread.join();
    }

    std::string LoopbackHost() {
        return "127.0.0.1:" + std::to_string(g_port);
    }

    std::string BlobUrlPath(char kind, uint32_t accountId, uint32_t appId, const std::string& name) {
        return std::string("/") + kind + "/" + std::to_string(accountId) + "/" +
               std::to_string(appId) + "/" + UrlEncodePath(name);
    }

    std::string MachineName() {
        char host[256] = {};
        if (::gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0')
            return "steamflipper";
        return host;
    }

    /* ----------------------------------------------------------------- rpcs --- */

    bool RpcBeginFileUpload(uint32_t appId, uint32_t accountId,
                            const uint8_t* req, uint32_t reqLen,
                            std::string& out, int32_t& eresult) {
        CCloud_ClientBeginFileUpload_Request in;
        if (!in.ParseFromArray(req, static_cast<int>(reqLen))) return false;

        std::string root, name;
        SplitRoot(in.filename(), root, name);
        if (!IsSafeRelativeName(name)) {
            LOG_WARN("CloudSaves: app={} refused upload of unsafe name '{}'", appId, in.filename());
            eresult = kEResultFail;   // answer rather than pass through: Valve would reject it too
            return true;
        }

        FileRecord rec;
        rec.size      = in.raw_file_size() ? in.raw_file_size() : in.file_size();
        rec.timestamp = in.time_stamp();
        rec.platforms = in.platforms_to_sync();
        rec.sha       = in.file_sha();
        rec.root      = root;
        {
            std::lock_guard lock(g_mutex);
            g_pending[PendingKey(accountId, appId, name)] = rec;
        }

        // file_size is the transferred size (equal to raw here, nothing is
        // compressed or encrypted on the way to a folder on this machine).
        const uint32_t blockLen = in.file_size() ? in.file_size() : in.raw_file_size();

        CCloud_ClientBeginFileUpload_Response resp;
        resp.set_encrypt_file(false);
        auto* block = resp.add_block_requests();
        block->set_url_host(LoopbackHost());
        block->set_url_path(BlobUrlPath('u', accountId, appId, name));
        block->set_use_https(false);
        block->set_http_method(kHttpMethodPut);
        block->set_block_offset(0);
        block->set_block_length(blockLen);

        LOG_DEBUG("CloudSaves: BeginFileUpload app={} '{}' {} bytes", appId, name, blockLen);
        eresult = kEResultOK;
        return resp.SerializeToString(&out);
    }

    bool RpcCommitFileUpload(uint32_t appId, uint32_t accountId,
                             const uint8_t* req, uint32_t reqLen,
                             std::string& out, int32_t& eresult) {
        CCloud_ClientCommitFileUpload_Request in;
        if (!in.ParseFromArray(req, static_cast<int>(reqLen))) return false;

        std::string root, name;
        SplitRoot(in.filename(), root, name);

        CCloud_ClientCommitFileUpload_Response resp;
        eresult = kEResultOK;

        if (!IsSafeRelativeName(name)) {
            resp.set_file_committed(false);
            return resp.SerializeToString(&out);
        }

        std::lock_guard lock(g_mutex);
        const std::string key = PendingKey(accountId, appId, name);
        auto pending = g_pending.find(key);

        if (!in.transfer_succeeded()) {
            // The client gave up mid-transfer. Drop the half-written blob rather
            // than publish it: the manifest would then point at truncated bytes.
            g_pending.erase(key);
            std::error_code ec;
            fs::remove(AppDir(accountId, appId) / name, ec);
            LOG_WARN("CloudSaves: app={} upload of '{}' failed on the client side", appId, name);
            resp.set_file_committed(false);
            return resp.SerializeToString(&out);
        }

        FileRecord rec = pending != g_pending.end() ? pending->second : FileRecord{};
        if (pending != g_pending.end()) g_pending.erase(pending);
        if (rec.root.empty()) rec.root = root;
        if (!in.file_sha().empty()) rec.sha = in.file_sha();

        // The blob arrived over the loopback endpoint; trust the filesystem for
        // its size rather than what the request announced.
        std::error_code ec;
        const auto onDisk = fs::file_size(AppDir(accountId, appId) / name, ec);
        if (ec) {
            LOG_WARN("CloudSaves: app={} committed '{}' but no blob was uploaded", appId, name);
            resp.set_file_committed(false);
            return resp.SerializeToString(&out);
        }
        rec.size = static_cast<uint32_t>(onDisk);
        if (rec.timestamp == 0) rec.timestamp = static_cast<uint64_t>(::time(nullptr));

        AppState& st = GetState(accountId, appId);
        st.files[name] = std::move(rec);
        // Outside a batch every commit is its own change; inside one the number
        // was already claimed at BeginAppUploadBatch and must not move, or the
        // client re-downloads what it just uploaded.
        if (st.activeBatchId == 0) st.changeNumber++;
        SaveManifest(accountId, appId, st);

        LOG_DEBUG("CloudSaves: CommitFileUpload app={} '{}' cn={}", appId, name, st.changeNumber);
        resp.set_file_committed(true);
        return resp.SerializeToString(&out);
    }

    bool RpcFileDownload(uint32_t appId, uint32_t accountId,
                         const uint8_t* req, uint32_t reqLen,
                         std::string& out, int32_t& eresult) {
        CCloud_ClientFileDownload_Request in;
        if (!in.ParseFromArray(req, static_cast<int>(reqLen))) return false;

        std::string root, name;
        SplitRoot(in.filename(), root, name);

        FileRecord rec;
        bool       have = false;
        if (IsSafeRelativeName(name)) {
            std::lock_guard lock(g_mutex);
            AppState& st = GetState(accountId, appId);
            auto it = st.files.find(name);
            if (it != st.files.end()) {
                rec  = it->second;
                have = true;
            }
        }
        if (!have) {
            // Truthful and non-fatal: the client treats a missing cloud file as
            // nothing to fetch and keeps whatever it has locally.
            LOG_DEBUG("CloudSaves: app={} has no stored copy of '{}'", appId, name);
            eresult = kEResultFileNotFound;
            return true;
        }

        CCloud_ClientFileDownload_Response resp;
        resp.set_appid(appId);
        resp.set_file_size(rec.size);
        resp.set_raw_file_size(rec.size);
        if (!rec.sha.empty()) resp.set_sha_file(rec.sha);
        resp.set_time_stamp(rec.timestamp);
        resp.set_is_explicit_delete(false);
        resp.set_url_host(LoopbackHost());
        resp.set_url_path(BlobUrlPath('d', accountId, appId, name));
        resp.set_use_https(false);
        resp.set_encrypted(false);

        LOG_DEBUG("CloudSaves: FileDownload app={} '{}' {} bytes", appId, name, rec.size);
        eresult = kEResultOK;
        return resp.SerializeToString(&out);
    }

    bool RpcDeleteFile(uint32_t appId, uint32_t accountId,
                       const uint8_t* req, uint32_t reqLen,
                       std::string& out, int32_t& eresult) {
        CCloud_ClientDeleteFile_Request in;
        if (!in.ParseFromArray(req, static_cast<int>(reqLen))) return false;

        std::string root, name;
        SplitRoot(in.filename(), root, name);

        out.clear();          // CCloud_ClientDeleteFile_Response is empty
        eresult = kEResultOK;
        if (!IsSafeRelativeName(name)) return true;

        std::lock_guard lock(g_mutex);
        g_pending.erase(PendingKey(accountId, appId, name));
        AppState& st = GetState(accountId, appId);
        if (st.files.erase(name)) {
            std::error_code ec;
            fs::remove(AppDir(accountId, appId) / name, ec);
            if (st.activeBatchId == 0) st.changeNumber++;
            SaveManifest(accountId, appId, st);
            LOG_DEBUG("CloudSaves: DeleteFile app={} '{}' cn={}", appId, name, st.changeNumber);
        }
        return true;
    }

    bool RpcGetAppFileChangelist(uint32_t appId, uint32_t accountId,
                                 const uint8_t* req, uint32_t reqLen, uint32_t respMaxLen,
                                 std::string& out, int32_t& eresult) {
        CCloud_GetAppFileChangelist_Request in;
        if (!in.ParseFromArray(req, static_cast<int>(reqLen))) return false;

        std::lock_guard lock(g_mutex);
        AppState& st = GetState(accountId, appId);

        CCloud_GetAppFileChangelist_Response resp;
        resp.set_current_change_number(st.changeNumber);
        resp.set_app_buildid_hwm(0);

        // is_only_delta says whether this list is the whole truth. Getting that
        // wrong destroys saves: a full inventory that omits a file tells the
        // client the cloud dropped it, and the client deletes the local copy to
        // match. So the full form is only ever sent when this store is the thing
        // the client synced against, which is exactly when its change number
        // came from here, which is exactly when it is below ours.
        if (in.synced_change_number() >= st.changeNumber) {
            if (in.synced_change_number() > st.changeNumber) {
                // Synced against a number this store never issued: the app has
                // real Valve cloud history, or the store was wiped. Adopt the
                // number so later batches stay above it, and report no change,
                // which leaves the local saves alone. The store refills as the
                // game writes.
                LOG_INFO("CloudSaves: app={} client is at cn={} beyond our cn={}, adopting it",
                         appId, in.synced_change_number(), st.changeNumber);
                st.changeNumber = in.synced_change_number();
                SaveManifest(accountId, appId, st);
                resp.set_current_change_number(st.changeNumber);
            }
            resp.set_is_only_delta(true);
            eresult = kEResultOK;
            LOG_DEBUG("CloudSaves: changelist app={} unchanged at cn={}", appId, st.changeNumber);
            return resp.SerializeToString(&out);
        }

        resp.set_is_only_delta(false);
        resp.add_machine_names(MachineName());

        // The wire form splits a path into a shared prefix table plus a leaf, so
        // rebuild "<root token><directories>/" per file and intern it.
        std::unordered_map<std::string, uint32_t> prefixIndex;
        bool truncated = false;

        for (const auto& [name, rec] : st.files) {
            const size_t slash = name.rfind('/');
            const std::string leaf   = slash == std::string::npos ? name : name.substr(slash + 1);
            const std::string prefix = rec.root + (slash == std::string::npos ? "" : name.substr(0, slash + 1));

            auto [it, inserted] = prefixIndex.emplace(prefix, static_cast<uint32_t>(prefixIndex.size()));
            if (inserted) resp.add_path_prefixes(prefix);

            auto* info = resp.add_files();
            info->set_file_name(leaf);
            if (!rec.sha.empty()) info->set_sha_file(rec.sha);
            info->set_time_stamp(rec.timestamp);
            info->set_raw_file_size(rec.size);
            info->set_persist_state(rec.persistState);
            info->set_platforms_to_sync(rec.platforms);
            info->set_path_prefix_index(it->second);
            info->set_machine_name_index(0);

            // The hook delivers the reply inside one synthesized packet, so the
            // body has a hard ceiling. Stop at it instead of emitting a message
            // that cannot be sent at all.
            if (resp.ByteSizeLong() > respMaxLen) {
                resp.mutable_files()->RemoveLast();
                if (inserted) {
                    resp.mutable_path_prefixes()->RemoveLast();
                    prefixIndex.erase(it);
                }
                truncated = true;
                break;
            }
        }
        if (truncated)
            LOG_WARN("CloudSaves: changelist app={} truncated to {} of {} files ({} byte cap)",
                     appId, resp.files_size(), st.files.size(), respMaxLen);

        LOG_DEBUG("CloudSaves: changelist app={} cn={} files={}",
                  appId, st.changeNumber, resp.files_size());
        eresult = kEResultOK;
        return resp.SerializeToString(&out);
    }

    bool RpcQuotaUsage(uint32_t appId, uint32_t accountId, std::string& out, int32_t& eresult) {
        uint32_t files = 0;
        uint64_t bytes = 0;
        {
            std::lock_guard lock(g_mutex);
            AppState& st = GetState(accountId, appId);
            files = static_cast<uint32_t>(st.files.size());
            for (const auto& [name, rec] : st.files) bytes += rec.size;
        }

        CCloud_ClientGetAppQuotaUsage_Response resp;
        resp.set_existing_files(files);
        resp.set_existing_bytes(bytes);
        resp.set_max_num_files(kQuotaMaxFiles);
        resp.set_max_num_bytes(kQuotaMaxBytes);

        LOG_DEBUG("CloudSaves: quota app={} {} files / {} bytes", appId, files, bytes);
        eresult = kEResultOK;
        return resp.SerializeToString(&out);
    }

    bool RpcBeginBatch(uint32_t appId, uint32_t accountId, std::string& out, int32_t& eresult) {
        const uint64_t batchId = g_nextBatchId.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard lock(g_mutex);
        AppState& st = GetState(accountId, appId);
        // The number handed out here is what the client records as synced, so it
        // has to be the one the batch's writes land under. Claim it up front.
        st.changeNumber++;
        st.activeBatchId = batchId;
        SaveManifest(accountId, appId, st);

        CCloud_BeginAppUploadBatch_Response resp;
        resp.set_batch_id(batchId);
        resp.set_app_change_number(st.changeNumber);

        LOG_DEBUG("CloudSaves: BeginAppUploadBatch app={} batch={} cn={}",
                  appId, batchId, st.changeNumber);
        eresult = kEResultOK;
        return resp.SerializeToString(&out);
    }

    bool RpcCompleteBatch(uint32_t appId, uint32_t accountId, std::string& out, int32_t& eresult) {
        std::lock_guard lock(g_mutex);
        AppState& st = GetState(accountId, appId);
        st.activeBatchId = 0;
        SaveManifest(accountId, appId, st);

        out.clear();          // CCloud_CompleteAppUploadBatch_Response is empty
        eresult = kEResultOK;
        LOG_DEBUG("CloudSaves: CompleteAppUploadBatch app={} cn={}", appId, st.changeNumber);
        return true;
    }

#endif // __linux__

} // namespace

/* ------------------------------------------------------------------ api --- */

bool Initialize(const char* steamInstallPath) {
#if defined(__linux__)
    // Same gate CloudRedirectHost uses, so switching the call site over does not
    // silently turn the feature on for anyone who left it off.
    if (!Config::GetCloudSettings().enabled) {
        LOG_INFO("CloudSaves: [cloud].enabled is false, native cloud saves disabled");
        return false;
    }
    if (!steamInstallPath || steamInstallPath[0] == '\0') {
        LOG_WARN("CloudSaves: empty Steam install path, cannot initialise");
        return false;
    }

    std::lock_guard lock(g_mutex);
    if (g_active.load(std::memory_order_acquire)) return true;

    const fs::path root = fs::path(steamInstallPath) / "steamflipper" / "cloudsaves";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (!fs::is_directory(root, ec)) {
        LOG_WARN("CloudSaves: cannot create {}", root.string());
        return false;
    }
    g_root = root.string();

    if (!StartHttp()) return false;

    g_appIds.clear();
    for (uint32_t id : ManifestAppIds()) g_appIds.insert(id);

    g_active.store(true, std::memory_order_release);
    LOG_INFO("CloudSaves: serving {} app(s) from {} (loopback port {})",
             g_appIds.size(), g_root, g_port);
    return true;
#else
    (void)steamInstallPath;
    return false;
#endif
}

/**
 * The apps that came from a Lua manifest, and only those.
 *
 * Seeding from LuaConfig::GetAllDepotIds() was wrong twice over. That set is
 * the keys of DepotKeySet, which are DEPOT ids, not app ids, so it claimed 1100
 * entries that are mostly not apps at all and could collide with a real app id.
 * Worse, anything it did match got its Steam Cloud pointed at a local folder,
 * including a game the account genuinely owns, whose real cloud saves would
 * then stop being the source of truth.
 *
 * A manifest is a <appid>.lua in a watched directory, so the file names are the
 * definition of "added via manifest". Anything else keeps Valve's cloud, which
 * is the safe default: the cost of missing an app is no cloud saves for it, the
 * cost of claiming one wrongly is someone's save history.
 */
static std::vector<uint32_t> ManifestAppIds() {
    std::vector<uint32_t> out;
    std::error_code ec;
    // g_root is <steam>/steamflipper/cloudsaves, so two levels up is the Steam
    // directory. Derived rather than stored separately so the two cannot drift.
    const std::string luaDir =
        (fs::path(g_root).parent_path().parent_path() / "config" / "stplug-in").string();
    for (const std::string& dir : LuaConfig::MergeWatchDirs(
             Config::GetLuaPaths(), luaDir)) {
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); ++it) {
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() != ".lua") continue;
            const std::string stem = it->path().stem().string();
            if (stem.empty() ||
                stem.find_first_not_of("0123456789") != std::string::npos) {
                continue;   // manifest.lua and other helpers are not apps
            }
            const unsigned long id = strtoul(stem.c_str(), nullptr, 10);
            if (id) out.push_back(static_cast<uint32_t>(id));
        }
    }
    return out;
}

bool IsActive() {
    return g_active.load(std::memory_order_acquire);
}

bool IsApp(uint32_t appId) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(g_mutex);
    return g_appIds.count(appId) != 0;
}

void SetApps(const uint32_t* appIds, uint32_t count) {
    std::lock_guard lock(g_mutex);
    g_appIds.clear();
    for (uint32_t i = 0; i < count && appIds; i++) g_appIds.insert(appIds[i]);
    LOG_DEBUG("CloudSaves: serving {} app(s)", g_appIds.size());
}

void SyncAppSet() {
    if (!g_active.load(std::memory_order_acquire)) return;
    const std::vector<uint32_t> appIds = ManifestAppIds();
    SetApps(appIds.empty() ? nullptr : appIds.data(), static_cast<uint32_t>(appIds.size()));
}

bool HandleRpc(const char* method, uint32_t appId, uint32_t accountId,
               const uint8_t* reqBody, uint32_t reqLen,
               uint8_t* respBuf, uint32_t respMaxLen,
               uint32_t* respLen, int32_t* eresult) {
#if defined(__linux__)
    if (!g_active.load(std::memory_order_acquire)) return false;
    if (!method || !respBuf || !respLen || !eresult) return false;
    // Every store path is keyed by account; without one there is nowhere to put
    // the save, and answering anyway would file it under the wrong user.
    if (accountId == 0) {
        LOG_WARN("CloudSaves: {} for app={} arrived before the account id was known", method, appId);
        return false;
    }
    if (!reqBody) reqLen = 0;

    std::string body;
    int32_t     result  = kEResultOK;
    bool        handled = false;

    if (std::strcmp(method, "Cloud.ClientBeginFileUpload#1") == 0) {
        handled = RpcBeginFileUpload(appId, accountId, reqBody, reqLen, body, result);
    } else if (std::strcmp(method, "Cloud.ClientCommitFileUpload#1") == 0) {
        handled = RpcCommitFileUpload(appId, accountId, reqBody, reqLen, body, result);
    } else if (std::strcmp(method, "Cloud.ClientFileDownload#1") == 0) {
        handled = RpcFileDownload(appId, accountId, reqBody, reqLen, body, result);
    } else if (std::strcmp(method, "Cloud.ClientDeleteFile#1") == 0) {
        handled = RpcDeleteFile(appId, accountId, reqBody, reqLen, body, result);
    } else if (std::strcmp(method, "Cloud.GetAppFileChangelist#1") == 0) {
        handled = RpcGetAppFileChangelist(appId, accountId, reqBody, reqLen, respMaxLen, body, result);
    } else if (std::strcmp(method, "Cloud.ClientGetAppQuotaUsage#1") == 0) {
        handled = RpcQuotaUsage(appId, accountId, body, result);
    } else if (std::strcmp(method, "Cloud.BeginAppUploadBatch#1") == 0) {
        handled = RpcBeginBatch(appId, accountId, body, result);
    } else if (std::strcmp(method, "Cloud.CompleteAppUploadBatchBlocking#1") == 0) {
        handled = RpcCompleteBatch(appId, accountId, body, result);

    // ── explicit no-ops ──────────────────────────────────────────
    // Each of these is answered rather than defaulted, because passing one
    // through reaches Valve, which rejects it for an app the account does not
    // own and fails the whole sync. All four responses are empty messages, so
    // an empty body plus EResultOK is the complete, correct reply.
    } else if (std::strcmp(method, "Cloud.ResumeAppSession#1") == 0 ||
               std::strcmp(method, "Cloud.SuspendAppSession#1") == 0) {
        // Session ownership only matters when a second machine can hold the
        // same save. A folder on this machine has no second machine.
        handled = true;
    } else if (std::strcmp(method, "Cloud.SignalAppLaunchIntent#1") == 0) {
        // The reply lists pending remote operations. There are never any: no
        // other client writes to this store, so the empty list is the truth.
        handled = true;
    } else if (std::strcmp(method, "Cloud.SignalAppExitSyncDone#1") == 0 ||
               std::strcmp(method, "Cloud.ClientConflictResolution#1") == 0) {
        // Notifications, so there is nothing to answer. Handled here for
        // completeness only: Hooks_NetPacket filters both out before this point
        // so Steam's own cloud state machine still sees them.
        handled = true;
    } else if (std::strcmp(method, "Cloud.ExternalStorageTransferReport#1") == 0) {
        // Telemetry about a transfer that never left the machine. Dropped.
        handled = true;
    }

    if (!handled) return false;   // unrecognised: caller chains to the original

    if (body.size() > respMaxLen) {
        LOG_WARN("CloudSaves: {} reply is {} bytes, over the {} byte cap",
                 method, body.size(), respMaxLen);
        return false;
    }
    if (!body.empty()) std::memcpy(respBuf, body.data(), body.size());
    *respLen  = static_cast<uint32_t>(body.size());
    *eresult  = result;
    return true;
#else
    (void)method; (void)appId; (void)accountId; (void)reqBody; (void)reqLen;
    (void)respBuf; (void)respMaxLen; (void)respLen; (void)eresult;
    return false;
#endif
}

void Shutdown() {
#if defined(__linux__)
    if (!g_active.exchange(false)) return;
    StopHttp();
    LOG_INFO("CloudSaves: shut down");
#endif
}

} // namespace CloudSaves
