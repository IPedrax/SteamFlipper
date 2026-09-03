#include "LuaFlipperDownload.h"

#include "SFPlatform/include/Http.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined(__linux__)
#include <zlib.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

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

std::string ProbeSources(const std::string& appId) {
#if defined(__linux__)
    if (!IsAppId(appId)) return JsonError("invalid appid");

    const SFPlatform::Http::Result r = Get(kProbeUrl + appId, 8192);

    std::string j = "{\"appid\":\"" + JsonEscape(appId) + "\",\"sources\":[";
    bool first = true;
    for (const Source& s : kSources) {
        // A dead probe is not fatal: check_apis is Ryuu's own host and Sushi is a
        // GitHub repo that does not depend on it, so report "unknown" and let the
        // user try rather than hiding a source that still works.
        std::string status = "unknown";
        if (r.ok && r.status == 200) {
            const std::string v = JsonField(r.body, s.name);
            status = v.empty() ? "unavailable" : v;
        }
        j += first ? "" : ",";
        first = false;
        j += "{\"name\":\"" + JsonEscape(s.name) +
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

    const Source* src = FindSource(source);
    if (!src) return JsonError("unknown source '" + source + "'");

    const std::string url = std::string(src->prefix) + appId + src->suffix;
    const SFPlatform::Http::Result r = Get(url, kMaxZipBytes);
    if (!r.ok)
        return JsonError(std::string("cannot reach ") + src->name);
    if (r.status != 200)
        return JsonError(std::string(src->name) + " returned HTTP " +
                         std::to_string(r.status));

    std::vector<CdEntry> entries;
    std::string err;
    if (!ReadCentralDirectory(r.body, entries, err))
        return JsonError(std::string(src->name) + ": " + err);

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
               JsonEscape(std::string(src->name) + " has no installable .lua or "
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

} // namespace LuaFlipperDownload
