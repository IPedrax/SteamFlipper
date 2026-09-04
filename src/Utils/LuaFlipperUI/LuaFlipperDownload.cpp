#include "LuaFlipperDownload.h"

#include "SFPlatform/include/Http.h"
#include "Utils/Config/Config.h"

#include <functional>
#include <cctype>
#include <mutex>

#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

#if defined(__linux__)
#include <zlib.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace LuaFlipperDownload {
namespace {

    /* -------------------------------------------------------------- json --- */

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

    std::string JsonError(const std::string& what) {
        return "{\"error\":\"" + JsonEscape(what) + "\"}";
    }

#if defined(__linux__)

    namespace fs = std::filesystem;

    /* ----------------------------------------------------------- sources --- */

    struct Source {
        const char* name;
        const char* prefix;   // the appid is appended verbatim
        const char* suffix;
    };

    // The two sources that need no account, and the only ones LuaTools itself
    // falls back to when api.json is absent. Kept in one table so the probe and
    // the download can never advertise different sets - that drift is the exact
    // failure mode LuaTools has between SourceMeta and api.json.
    //
    // Ryuu is plaintext HTTP to a bare IP: no TLS, no certificate identity. An
    // installed .lua is later executed by the loader, so this source is only as
    // trustworthy as the network path to it. Nothing below trusts the bytes
    // beyond the archive's own CRC.
    constexpr Source kSources[] = {
        { "Ryuu",  "http://167.235.229.108/", "" },
        { "Sushi", "https://raw.githubusercontent.com/sushi-dev55-alt/"
                   "sushitools-games-repo-alt/refs/heads/main/", ".zip" },
    };

    constexpr const char* kProbeUrl = "http://167.235.229.108/check_apis?appid=";

    /*
     * Hubcap (hubcapmanifest.com), the one source that takes no proxy.
     *
     * Not a row in kSources because it does not fit that shape: the URL carries
     * the user's own API key, and it answers a free existence check the shared
     * probe knows nothing about. Downloads count against that key's own daily
     * limit rather than any pool this project shares, which is exactly why the
     * key is the user's to supply and is never defaulted.
     *
     * The name matches LuaTools' own catalog entry, so a manifest installed by
     * either is attributed the same way.
     */
    /*
     * The sources lua.tools serves through its own proxy.
     *
     * These are not reachable any other way: the endpoint answers 401 to
     * everything without a session, and unlike Ryuu and Sushi they publish no
     * address of their own that LuaTools falls back to. So they appear in the
     * list either way, and say they need a sign-in rather than pretending the
     * app is not carried.
     *
     * Ryuu and Sushi are deliberately not here. Both are already fetched
     * directly and keylessly, and routing them through the proxy would spend a
     * download from the user's daily 25 to get the same bytes.
     */
    constexpr const char* kProxySources[] = { "Luie", "TwentyTwo Cloud", "Skyflare" };
    constexpr const char* kProxyUrl = "https://lua.tools/api/manifest/download";

    std::mutex g_tokenLock;
    std::function<std::string()> g_tokenProvider;

    std::string BearerToken() {
        std::function<std::string()> f;
        { std::lock_guard<std::mutex> lock(g_tokenLock); f = g_tokenProvider; }
        return f ? f() : std::string();
    }

    // Source names carry spaces and brackets, and they go in a query string.
    std::string UrlEncode(const std::string& in) {
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

    bool IsProxySource(const std::string& name) {
        for (const char* p : kProxySources) if (name == p) return true;
        return false;
    }

    constexpr const char* kHubcapName = "Sadie (Hubcap)";
    constexpr const char* kHubcapBase = "https://hubcapmanifest.com";

    // "smm_" followed by 96 lowercase hex. Checked before the key goes into a
    // URL, so a typo is answered here rather than as someone else's 401.
    bool IsHubcapKey(const std::string& k) {
        if (k.size() != 100 || k.compare(0, 4, "smm_") != 0) return false;
        for (size_t i = 4; i < k.size(); i++) {
            const char c = k[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    // A pack runs to a few MB, so Http::Execute's 256 KiB default would silently
    // truncate one. This is headroom rather than a target; anything cut short at
    // the cap fails the end-of-central-directory scan instead of installing junk.
    constexpr uint32_t kMaxZipBytes = 32u * 1024u * 1024u;
    // Per-entry ceiling, so a crafted archive cannot talk a 32-bit process into
    // an allocation it will not survive.
    constexpr uint32_t kMaxEntryBytes = 64u * 1024u * 1024u;

    const Source* FindSource(const std::string& name) {
        for (const Source& s : kSources)
            if (name == s.name) return &s;
        return nullptr;
    }

    // The appid is pasted straight into a URL, so this is the injection guard as
    // much as a sanity check. u32 tops out at ten digits.
    bool IsAppId(const std::string& s) {
        if (s.empty() || s.size() > 10) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    }

    // Value of a flat JSON string field. The only document parsed here is the
    // check_apis reply, a flat map of source name to status, so a real parser
    // would buy nothing.
    std::string JsonField(const std::string& doc, const std::string& key) {
        const std::string needle = "\"" + key + "\"";
        const size_t k = doc.find(needle);
        if (k == std::string::npos) return {};
        const size_t c = doc.find(':', k + needle.size());
        if (c == std::string::npos) return {};
        const size_t q = doc.find('"', c);
        if (q == std::string::npos) return {};
        const size_t e = doc.find('"', q + 1);
        if (e == std::string::npos) return {};
        return doc.substr(q + 1, e - q - 1);
    }

    std::string JsonArray(const std::vector<std::string>& v) {
        std::string j = "[";
        for (size_t i = 0; i < v.size(); i++) {
            if (i) j += ",";
            j += "\"" + JsonEscape(v[i]) + "\"";
        }
        return j + "]";
    }

    SFPlatform::Http::Result Get(const std::string& url, uint32_t maxBytes) {
        // The receive timeout is a stall window in the curl backend, not a
        // deadline on the whole transfer, so a generous value costs nothing on a
        // healthy connection and keeps slow mirrors alive.
        return SFPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, nullptr,
                                         5000, 5000, 10000, 30000, maxBytes);
    }

    /* ------------------------------------------------------------- unzip --- */

    uint16_t R16(const std::string& b, size_t at) {
        return static_cast<uint16_t>(static_cast<unsigned char>(b[at]) |
                                     (static_cast<unsigned char>(b[at + 1]) << 8));
    }

    uint32_t R32(const std::string& b, size_t at) {
        return static_cast<uint32_t>(static_cast<unsigned char>(b[at])) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24);
    }

    bool Sig(const std::string& b, size_t at, const char* four) {
        return at + 4 <= b.size() && std::memcmp(b.data() + at, four, 4) == 0;
    }

    struct CdEntry {
        std::string name;
        uint32_t    crc          = 0;
        uint32_t    compressed   = 0;
        uint32_t    uncompressed = 0;
        uint32_t    localOffset  = 0;
        uint16_t    method       = 0;
        uint16_t    flags        = 0;
    };

    // The end of central directory record sits at the tail, but a trailing
    // archive comment can push it back by up to 64 KiB, so scan backwards. Its
    // comment length has to account for exactly the bytes that follow, and that
    // is what tells the real record apart from the same four bytes appearing
    // inside compressed file data.
    size_t FindEocd(const std::string& z) {
        if (z.size() < 22) return std::string::npos;
        const size_t span = 22u + 65535u;
        const size_t stop = z.size() > span ? z.size() - span : 0;
        for (size_t i = z.size() - 22; ; --i) {
            if (Sig(z, i, "PK\x05\x06") &&
                i + 22u + static_cast<size_t>(R16(z, i + 20)) == z.size())
                return i;
            if (i == stop) break;
        }
        return std::string::npos;
    }

    // Walks the central directory rather than scanning for local file headers:
    // an entry written with a data descriptor (flag bit 3) carries zeroed sizes
    // and CRC in its local header, so only the central copy is authoritative.
    bool ReadCentralDirectory(const std::string& z, std::vector<CdEntry>& out,
                              std::string& err) {
        const size_t eocd = FindEocd(z);
        if (eocd == std::string::npos) {
            err = "not a zip archive (no end of central directory record)";
            return false;
        }

        const uint16_t count  = R16(z, eocd + 10);
        const uint32_t cdSize = R32(z, eocd + 12);
        const uint32_t cdOff  = R32(z, eocd + 16);
        // These sentinels mean the real values live in a zip64 record. Say so
        // rather than walking a bogus offset.
        if (count == 0xFFFFu || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) {
            err = "zip64 archives are not supported";
            return false;
        }
        if (static_cast<uint64_t>(cdOff) + cdSize > z.size()) {
            err = "central directory runs past the end of the archive";
            return false;
        }

        size_t p = cdOff;
        for (uint16_t i = 0; i < count; i++) {
            if (p + 46u > z.size() || !Sig(z, p, "PK\x01\x02")) {
                err = "malformed central directory";
                return false;
            }
            CdEntry e;
            e.flags        = R16(z, p + 8);
            e.method       = R16(z, p + 10);
            e.crc          = R32(z, p + 16);
            e.compressed   = R32(z, p + 20);
            e.uncompressed = R32(z, p + 24);
            e.localOffset  = R32(z, p + 42);
            const size_t nameLen  = R16(z, p + 28);
            const size_t extraLen = R16(z, p + 30);
            const size_t cmtLen   = R16(z, p + 32);
            if (p + 46u + nameLen + extraLen + cmtLen > z.size()) {
                err = "malformed central directory";
                return false;
            }
            e.name.assign(z, p + 46u, nameLen);
            out.push_back(std::move(e));
            p += 46u + nameLen + extraLen + cmtLen;
        }
        return true;
    }

    // Entry data starts after the LOCAL header's own name and extra fields. The
    // extra field routinely differs in size from the central directory copy, so
    // the local lengths are the only ones that give the right offset.
    bool EntryData(const std::string& z, const CdEntry& e, size_t& at,
                   std::string& err) {
        if (e.localOffset > z.size() || z.size() - e.localOffset < 30u ||
            !Sig(z, e.localOffset, "PK\x03\x04")) {
            err = "bad local file header";
            return false;
        }
        at = e.localOffset + 30u + R16(z, e.localOffset + 26) +
             R16(z, e.localOffset + 28);
        if (at > z.size() || z.size() - at < e.compressed) {
            err = "entry data runs past the end of the archive";
            return false;
        }
        return true;
    }

    bool Inflate(const char* src, uint32_t srcLen, uint32_t outLen,
                 std::string& out) {
        out.assign(outLen, '\0');
        z_stream zs{};
        // Raw deflate: a zip entry carries no zlib header of its own.
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(src));
        zs.avail_in  = srcLen;
        zs.next_out  = reinterpret_cast<Bytef*>(out.data());
        zs.avail_out = outLen;
        const int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        // Z_FINISH into an exactly-sized buffer either completes in one call or
        // the entry lied about its uncompressed size.
        return rc == Z_STREAM_END && zs.total_out == outLen;
    }

    /* ----------------------------------------------------------- install --- */

    std::string Base(const std::string& name) {
        const size_t slash = name.find_last_of('/');
        return slash == std::string::npos ? name : name.substr(slash + 1);
    }

    bool EndsWith(const std::string& s, const char* suffix) {
        const size_t n = std::strlen(suffix);
        return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
    }

    // .lua grants ownership, .manifest gives the depot its file list; one without
    // the other installs nothing usable, which is why both land in the same run.
    // Extensions are matched exactly because the loader and the depot cache both
    // look for lowercase names, so an oddly-cased entry would install to a path
    // nothing reads.
    const fs::path* DestinationFor(const std::string& base, const fs::path& luaDir,
                                   const fs::path& manifestDir) {
        if (EndsWith(base, ".lua"))      return &luaDir;
        if (EndsWith(base, ".manifest")) return &manifestDir;
        return nullptr;
    }

    bool WriteAtomic(const fs::path& dir, const std::string& name,
                     const std::string& data, std::string& err) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!fs::is_directory(dir, ec)) {
            err = "cannot create " + dir.string();
            return false;
        }

        // Temp file inside the target directory so the rename is same-filesystem
        // and therefore atomic: Steam never sees a half-written manifest, and a
        // failed download cannot destroy the copy already installed.
        std::string tmp = (dir / (name + ".sfXXXXXX")).string();
        const int fd = ::mkstemp(tmp.data());
        if (fd < 0) {
            err = std::string("mkstemp: ") + std::strerror(errno);
            return false;
        }
        ::fchmod(fd, 0644);   // mkstemp opens 0600; a manifest is not a secret

        bool ok = true;
        for (size_t off = 0; off < data.size(); ) {
            const ssize_t w = ::write(fd, data.data() + off, data.size() - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                ok = false;
                err = std::string("write: ") + std::strerror(errno);
                break;
            }
            off += static_cast<size_t>(w);
        }
        // Flush before the rename, or a crash can publish the name with no bytes
        // behind it.
        if (ok && ::fsync(fd) != 0) {
            ok = false;
            err = std::string("fsync: ") + std::strerror(errno);
        }
        ::close(fd);

        const std::string target = (dir / name).string();
        if (ok && ::rename(tmp.c_str(), target.c_str()) != 0) {
            ok = false;
            err = std::string("rename: ") + std::strerror(errno);
        }
        if (!ok) ::unlink(tmp.c_str());
        return ok;
    }

#endif // __linux__

} // namespace

std::vector<std::string> EffectiveOrder() {
#if defined(__linux__)
    std::vector<std::string> order;
    auto known = [](const std::string& n) {
        if (n == kHubcapName) return true;
        for (const Source& s : kSources) if (n == s.name) return true;
        return IsProxySource(n);
    };
    // The configured preference first, ignoring names this build does not have:
    // a typo should reorder nothing rather than invent a source.
    for (const std::string& want : Config::GetSourceOrder())
        if (known(want) && std::find(order.begin(), order.end(), want) == order.end())
            order.push_back(want);
    for (const Source& src : kSources)
        if (std::find(order.begin(), order.end(), src.name) == order.end())
            order.push_back(src.name);
    for (const char* p : kProxySources)
        if (std::find(order.begin(), order.end(), p) == order.end())
            order.push_back(p);
    if (std::find(order.begin(), order.end(), kHubcapName) == order.end())
        order.push_back(kHubcapName);
    return order;
#else
    return {};
#endif
}

void SetTokenProvider(std::function<std::string()> provider) {
    std::lock_guard<std::mutex> lock(g_tokenLock);
    g_tokenProvider = std::move(provider);
}

std::string ProbeSources(const std::string& appId) {
#if defined(__linux__)
    if (!IsAppId(appId)) return JsonError("invalid appid");

    const SFPlatform::Http::Result r = Get(kProbeUrl + appId, 8192);

    /*
     * Ordered before it is emitted, because the order IS the preference: every
     * caller that installs walks this list top down and takes the first source
     * that works, so putting the user's choice first here is the whole feature.
     *
     * A configured name that this build does not know matches nothing and is
     * skipped; a known source left out of the list keeps its built-in position
     * behind the ones that were named. Neither can disable a source, which is
     * what makes a typo in the list harmless.
     */
    const std::vector<std::string> order = EffectiveOrder();
    const bool signedIn = !BearerToken().empty();

    std::string j = "{\"appid\":\"" + JsonEscape(appId) + "\",\"sources\":[";
    bool first = true;
    for (const std::string& name : order) {
        std::string status;

        if (IsProxySource(name)) {
            // The probe host knows some of these by name; the rest are simply
            // unknown until asked. Either way a sign-in is the gate, and that
            // is the more useful thing to say.
            if (!signedIn) status = "needs sign-in";
            else if (r.ok && r.status == 200) {
                const std::string v = JsonField(r.body, name);
                status = v.empty() ? "unknown" : v;
            } else status = "unknown";
            j += first ? "" : ",";
            first = false;
            j += "{\"name\":\"" + JsonEscape(name) +
                 "\",\"status\":\"" + JsonEscape(status) + "\"}";
            continue;
        }

        if (name != kHubcapName) {
            // A dead probe is not fatal: check_apis is Ryuu's own host and Sushi
            // is a GitHub repo that does not depend on it, so report "unknown"
            // and let the user try rather than hiding a source that still works.
            status = "unknown";
            if (r.ok && r.status == 200) {
                const std::string v = JsonField(r.body, name);
                status = v.empty() ? "unavailable" : v;
            }
            j += first ? "" : ",";
            first = false;
            j += "{\"name\":\"" + JsonEscape(name) +
                 "\",\"status\":\"" + JsonEscape(status) + "\"}";
            continue;
        }

        // Hubcap is asked separately: its status comes from its own free
        // endpoint rather than the shared probe, which does not list it. Without
        // a key the honest answer is that one is needed, not "unavailable" - the
        // manifest may well be there.
        const std::string key = Config::GetHubcapKey();
        status = "needs key";
        if (IsHubcapKey(key)) {
            const std::string url = std::string(kHubcapBase) + "/api/v1/status/" + appId;
            const std::wstring auth = L"Authorization: Bearer " +
                                      std::wstring(key.begin(), key.end()) + L"\r\n";
            const SFPlatform::Http::Result hr = SFPlatform::Http::Execute(
                L"GET", url.c_str(), nullptr, 0, auth.c_str(),
                5000, 5000, 10000, 15000, 8192);
            if (!hr.ok)                 status = "unknown";
            else if (hr.status == 401)  status = "bad key";
            else if (hr.status != 200)  status = "unavailable";
            else status = hr.body.find("\"manifest_file_exists\":true") != std::string::npos
                          ? "available" : "unavailable";
        } else if (!key.empty()) {
            status = "bad key";
        }
        j += first ? "" : ",";
        first = false;
        j += "{\"name\":\"" + JsonEscape(kHubcapName) +
             "\",\"status\":\"" + JsonEscape(status) + "\"}";
    }

    j += "]}";
    return j;
#else
    (void)appId;
    return JsonError("unsupported platform");
#endif
}

std::string Install(const std::string& appId, const std::string& source,
                    const std::string& steamPath) {
#if defined(__linux__)
    if (!IsAppId(appId))   return JsonError("invalid appid");
    if (steamPath.empty()) return JsonError("no Steam path");

    // Hubcap is fetched with the user's key against its own host; everything
    // below this point is the same archive handling as the keyless sources.
    const bool hubcap = (source == kHubcapName);
    std::string label = source;
    std::string url;

    if (IsProxySource(source)) {
        const std::string token = BearerToken();
        if (token.empty())
            return JsonError("Sign in to lua.tools on the Fixes page to use " +
                             source + ".");
        url = std::string(kProxyUrl) + "?appid=" + appId + "&source=" +
              UrlEncode(source);
        label = source;
    } else if (hubcap) {
        const std::string key = Config::GetHubcapKey();
        if (key.empty())
            return JsonError("Hubcap needs your own API key. Put it in "
                             "steamflipper.toml as [hubcap] key = \"smm_...\" "
                             "and restart Steam.");
        if (!IsHubcapKey(key))
            return JsonError("That does not look like a Hubcap key: they are "
                             "\"smm_\" followed by 96 hex characters.");
        url = std::string(kHubcapBase) + "/api/v1/manifest/" + appId +
              "?api_key=" + key;
    } else {
        const Source* src = FindSource(source);
        if (!src) return JsonError("unknown source '" + source + "'");
        label = src->name;
        url = std::string(src->prefix) + appId + src->suffix;
    }

    // The proxy is the only source that needs a header; everything else is a
    // plain GET, and the token must not leak onto hosts that never asked for it.
    SFPlatform::Http::Result r;
    if (IsProxySource(source)) {
        const std::string token = BearerToken();
        const std::wstring headers =
            std::wstring(L"Authorization: Bearer ") +
            std::wstring(token.begin(), token.end()) + L"\r\n" +
            L"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:154.0) "
            L"Gecko/20100101 Firefox/154.0\r\n";
        r = SFPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0,
                                      headers.c_str(), 5000, 5000, 10000, 60000,
                                      kMaxZipBytes);
        if (r.ok && (r.status == 401 || r.status == 403))
            return JsonError("lua.tools refused the session. Sign in again on "
                             "the Fixes page.");
        if (r.ok && r.status == 429)
            return JsonError("The daily lua.tools download limit is spent. It is "
                             "25 a day, shared with fix downloads.");
    } else {
        r = Get(url, kMaxZipBytes);
    }
    if (!r.ok)
        return JsonError("cannot reach " + label);
    if (r.status != 200) {
        // Hubcap answers these three specifically, and each has a different
        // thing for the user to do about it.
        if (hubcap && r.status == 401)
            return JsonError("Hubcap rejected the key. Check it in Config, or "
                             "generate a new one.");
        if (hubcap && r.status == 429)
            return JsonError("This key's daily Hubcap limit is spent.");
        if (hubcap && r.status == 404)
            return JsonError("Hubcap has no manifest for this app.");
        return JsonError(label + " returned HTTP " + std::to_string(r.status));
    }

    std::vector<CdEntry> entries;
    std::string err;
    if (!ReadCentralDirectory(r.body, entries, err))
        return JsonError(label + ": " + err);

    const fs::path luaDir      = fs::path(steamPath) / "config" / "stplug-in";
    const fs::path manifestDir = fs::path(steamPath) / "depotcache";

    std::vector<std::string> installed;
    std::vector<std::string> rejected;

    for (const CdEntry& e : entries) {
        if (e.name.empty()) continue;

        // Checked before anything else, and for every entry rather than only the
        // ones destined for disk, so a hostile archive is reported in full even
        // though the extension filter below would have dropped most of it.
        if (e.name.find("..") != std::string::npos || e.name[0] == '/') {
            rejected.push_back(e.name + ": rejected, name escapes the install directory");
            continue;
        }

        const std::string base = Base(e.name);
        const fs::path* dir = DestinationFor(base, luaDir, manifestDir);
        if (!dir) continue;   // not a .lua or .manifest: not ours

        if (e.flags & 0x1) {
            rejected.push_back(base + ": encrypted");
            continue;
        }
        if (e.uncompressed > kMaxEntryBytes) {
            rejected.push_back(base + ": implausibly large (" +
                               std::to_string(e.uncompressed) + " bytes)");
            continue;
        }

        size_t at = 0;
        if (!EntryData(r.body, e, at, err)) {
            rejected.push_back(base + ": " + err);
            continue;
        }

        std::string data;
        if (e.method == 0) {
            if (e.compressed != e.uncompressed) {
                rejected.push_back(base + ": stored entry has mismatched sizes");
                continue;
            }
            data.assign(r.body, at, e.compressed);
        } else if (e.method == 8) {
            if (!Inflate(r.body.data() + at, e.compressed, e.uncompressed, data)) {
                rejected.push_back(base + ": inflate failed");
                continue;
            }
        } else {
            rejected.push_back(base + ": unsupported compression method " +
                               std::to_string(e.method));
            continue;
        }

        // The pack arrives over a plaintext link from a source with no signing,
        // so the archive's own checksum is the only integrity signal there is.
        // It still catches the realistic failure - a truncated or corrupt body.
        const uLong crc = crc32(crc32(0L, Z_NULL, 0),
                                reinterpret_cast<const Bytef*>(data.data()),
                                static_cast<uInt>(data.size()));
        if (crc != e.crc) {
            rejected.push_back(base + ": CRC mismatch");
            continue;
        }

        if (!WriteAtomic(*dir, base, data, err)) {
            rejected.push_back(base + ": " + err);
            continue;
        }
        installed.push_back(base);
    }

    if (installed.empty()) {
        // Carry the per-entry reasons along, since they are the only explanation
        // of why a 200 with a valid archive still installed nothing.
        return "{\"error\":\"" +
               JsonEscape(label + " has no installable .lua or "
                          ".manifest for appid " + appId) +
               "\",\"rejected\":" + JsonArray(rejected) + "}";
    }

    return "{\"ok\":true,\"installed\":" + std::to_string(installed.size()) +
           ",\"files\":" + JsonArray(installed) +
           ",\"rejected\":" + JsonArray(rejected) + "}";
#else
    (void)appId;
    (void)source;
    (void)steamPath;
    return JsonError("unsupported platform");
#endif
}

namespace {

/**
 * Value of the next "key" "value" pair in a Valve KeyValues document.
 *
 * `at` walks forward, so repeated calls enumerate every occurrence, which is
 * what libraryfolders.vdf needs: it holds one "path" per library. Not a
 * KeyValues parser -- it does not know about nesting, and would happily read a
 * "path" belonging to something else. It is enough for the two documents it is
 * pointed at, which have one key of each name per block, and every candidate it
 * produces is checked against the filesystem before it is used.
 */
std::string VdfValue(const std::string& doc, const std::string& key, size_t& at) {
    const std::string needle = "\"" + key + "\"";
    const size_t k = doc.find(needle, at);
    if (k == std::string::npos) { at = doc.size(); return {}; }
    const size_t q = doc.find('"', k + needle.size());
    if (q == std::string::npos) { at = doc.size(); return {}; }
    const size_t e = doc.find('"', q + 1);
    if (e == std::string::npos) { at = doc.size(); return {}; }
    at = e + 1;
    return doc.substr(q + 1, e - q - 1);
}

std::string ReadAll(const fs::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

} // namespace

std::vector<std::string> Libraries(const std::string& steamPath) {
#if defined(__linux__)
    // The Steam root is a library itself and is not listed as one in every
    // install, so it goes in first rather than being waited for.
    std::vector<std::string> roots{ steamPath };
    const std::string doc =
        ReadAll(fs::path(steamPath) / "steamapps" / "libraryfolders.vdf");
    for (size_t at = 0;;) {
        const std::string path = VdfValue(doc, "path", at);
        if (path.empty()) break;
        if (std::find(roots.begin(), roots.end(), path) == roots.end())
            roots.push_back(path);
    }
    return roots;
#else
    (void)steamPath;
    return {};
#endif
}

std::string GameDir(const std::string& appId, const std::string& steamPath) {
#if defined(__linux__)
    std::error_code ec;
    for (const std::string& lib : Libraries(steamPath)) {
        const fs::path root(lib);
        const fs::path acf =
            root / "steamapps" / ("appmanifest_" + appId + ".acf");
        if (!fs::exists(acf, ec)) continue;
        const std::string doc = ReadAll(acf);
        size_t at = 0;
        const std::string installdir = VdfValue(doc, "installdir", at);
        if (installdir.empty()) continue;
        // The manifest can outlive the files: an interrupted move leaves the acf
        // behind on the source library, so the directory is what decides.
        const fs::path dir = root / "steamapps" / "common" / installdir;
        if (fs::is_directory(dir, ec)) return dir.string();
    }
    return {};
#else
    (void)appId;
    (void)steamPath;
    return {};
#endif
}

std::string Apply(const std::string& archivePath, const std::string& gameDir) {
#if defined(__linux__)
    std::error_code ec;
    if (!fs::is_directory(gameDir, ec))
        return JsonError("the game folder " + gameDir + " is not there");

    std::string z;
    {
        std::FILE* f = std::fopen(archivePath.c_str(), "rb");
        if (!f) return JsonError("cannot read " + archivePath);
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) z.append(buf, n);
        std::fclose(f);
    }

    std::vector<CdEntry> entries;
    std::string err;
    if (!ReadCentralDirectory(z, entries, err)) return JsonError(err);

    std::vector<std::string> applied, rejected, runnable;
    size_t backed = 0, locked = 0;

    // Extracting is the whole job for most fixes and the first step for the
    // rest, and the difference is not in the archive's structure -- it is in
    // prose written for a person. What can be spotted is the shape of the
    // second step: a script or an installer sitting in the archive. Named, not
    // acted on; running one is not this module's call.
    auto looksRunnable = [](const std::string& n) {
        static const char* kExts[] = { ".cmd", ".bat", ".vbs", ".exe", ".reg",
                                       ".ps1", ".sh" };
        if (n.find('/') != std::string::npos) return false;   // top level only
        for (const char* ext : kExts)
            if (EndsWith(n, ext)) return true;
        return false;
    };

    for (const CdEntry& e : entries) {
        if (e.name.empty() || e.name.back() == '/') continue;   // a directory

        // Anything that could resolve outside the game folder is refused before
        // it is read, not sanitised: a fix archive has no business naming a
        // parent directory, and an attempt to is worth showing rather than
        // silently repairing. Backslashes go too - they are legal in a zip name
        // and mean nothing to this filesystem, so they would land as one long
        // filename rather than the path the archive intended.
        if (e.name.find("..") != std::string::npos || e.name[0] == '/' ||
            e.name.find('\\') != std::string::npos) {
            rejected.push_back(e.name + ": rejected, name escapes the game folder");
            continue;
        }

        if (e.flags & 0x1) { locked++; continue; }
        if (e.uncompressed > kMaxEntryBytes) {
            rejected.push_back(e.name + ": implausibly large (" +
                               std::to_string(e.uncompressed) + " bytes)");
            continue;
        }

        size_t at = 0;
        if (!EntryData(z, e, at, err)) {
            rejected.push_back(e.name + ": " + err);
            continue;
        }

        std::string data;
        if (e.method == 0) {
            if (e.compressed != e.uncompressed) {
                rejected.push_back(e.name + ": stored entry has mismatched sizes");
                continue;
            }
            data.assign(z, at, e.compressed);
        } else if (e.method == 8) {
            if (!Inflate(z.data() + at, e.compressed, e.uncompressed, data)) {
                rejected.push_back(e.name + ": inflate failed");
                continue;
            }
        } else {
            rejected.push_back(e.name + ": unsupported compression method " +
                               std::to_string(e.method));
            continue;
        }

        const uLong crc = crc32(crc32(0L, Z_NULL, 0),
                                reinterpret_cast<const Bytef*>(data.data()),
                                static_cast<uInt>(data.size()));
        if (crc != e.crc) {
            rejected.push_back(e.name + ": CRC mismatch");
            continue;
        }

        const fs::path dest = fs::path(gameDir) / e.name;
        // Copied, not renamed: WriteAtomic can still fail, and moving the
        // original out of the way first would turn a failed write into a
        // missing file. Only the first apply backs anything up, so applying a
        // second fix cannot overwrite the copy of what the game shipped.
        if (fs::exists(dest, ec)) {
            fs::path bak = dest;
            bak += ".sfbak";
            if (!fs::exists(bak, ec)) {
                fs::copy_file(dest, bak, ec);
                if (!ec) backed++;
            }
        }

        if (!WriteAtomic(dest.parent_path(), dest.filename().string(), data, err)) {
            rejected.push_back(e.name + ": " + err);
            continue;
        }
        applied.push_back(e.name);
        if (looksRunnable(e.name)) runnable.push_back(e.name);
    }

    if (applied.empty() && locked) {
        return JsonError("This archive is password protected, so it can only be "
                         "unpacked by hand. Fixes mirrored from online-fix.me "
                         "usually take \"online-fix.me\" as the password.");
    }
    if (applied.empty()) {
        return "{\"error\":\"Nothing in the archive could be applied\",\"rejected\":" +
               JsonArray(rejected) + "}";
    }
    if (locked)
        rejected.push_back(std::to_string(locked) +
                           " encrypted entries were left in the archive");

    return "{\"ok\":true,\"applied\":" + std::to_string(applied.size()) +
           ",\"backed\":" + std::to_string(backed) +
           ",\"dir\":\"" + JsonEscape(gameDir) + "\",\"runnable\":" +
           JsonArray(runnable) + ",\"rejected\":" + JsonArray(rejected) + "}";
#else
    (void)archivePath;
    (void)gameDir;
    return JsonError("unsupported platform");
#endif
}

} // namespace LuaFlipperDownload
