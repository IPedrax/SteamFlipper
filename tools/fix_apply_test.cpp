// Runnable check for the two halves of "extract this fix over the game":
// LuaFlipperDownload::GameDir, which turns an appid into an install folder, and
// Apply, which unpacks an archive into it.
//
// Linked against the object the build already produced, so it exercises the
// shipping code rather than a copy of it. The three symbols stubbed below are
// the only things that translation unit needs from elsewhere; neither function
// under test touches them.
//
//   g++ -m32 -std=c++20 -I src tools/fix_apply_test.cpp \
//       build/32/CMakeFiles/SteamFlipper.dir/Utils/LuaFlipperUI/LuaFlipperDownload.cpp.o \
//       -lz -o /tmp/fix_apply_test
//   /tmp/fix_apply_test [a downloaded fix .zip]
//
// The folder lookup runs against the Steam install on this machine, which is
// the point: libraryfolders.vdf is Valve's format and the only trustworthy
// sample of it is a real one. Everything else builds its own archives, since no
// packer will produce the hostile names the extractor has to refuse.
#include "Utils/LuaFlipperUI/LuaFlipperDownload.h"
#include "SFPlatform/include/Http.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// The three things the object needs that live in other translation units.
// Apply touches none of them; they only have to resolve.
namespace Config {
    std::vector<std::string> GetSourceOrder() { return {}; }
    std::string GetHubcapKey() { return {}; }
}
namespace SFPlatform { namespace Http {
    Result Execute(const wchar_t*, const char*, const void*, unsigned int,
                   const wchar_t*, unsigned int, unsigned int, unsigned int,
                   unsigned int, unsigned int) { return {}; }
}}

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

static bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// A zip built by hand, so the escape guard can be tested: no packer will emit
// "../escaped.txt" for us. Stored entries only, which keeps this to CRC + two
// headers per file.
static uint32_t crc_of(const std::string& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (~(c & 1) + 1));
    }
    return ~c;
}
static void put16(std::string& z, uint16_t v) {
    z += char(v & 0xFF); z += char(v >> 8);
}
static void put32(std::string& z, uint32_t v) {
    for (int i = 0; i < 4; i++) z += char((v >> (8 * i)) & 0xFF);
}
static std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string z;
    std::vector<uint32_t> offsets;
    for (auto& f : files) {
        offsets.push_back(uint32_t(z.size()));
        z += "PK\x03\x04"; put16(z, 10); put16(z, 0); put16(z, 0);
        put16(z, 0); put16(z, 0);
        put32(z, crc_of(f.second));
        put32(z, uint32_t(f.second.size())); put32(z, uint32_t(f.second.size()));
        put16(z, uint16_t(f.first.size())); put16(z, 0);
        z += f.first; z += f.second;
    }
    const uint32_t cdOff = uint32_t(z.size());
    for (size_t i = 0; i < files.size(); i++) {
        z += "PK\x01\x02"; put16(z, 20); put16(z, 10); put16(z, 0); put16(z, 0);
        put16(z, 0); put16(z, 0);
        put32(z, crc_of(files[i].second));
        put32(z, uint32_t(files[i].second.size()));
        put32(z, uint32_t(files[i].second.size()));
        put16(z, uint16_t(files[i].first.size()));
        put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0);
        put32(z, 0); put32(z, offsets[i]);
        z += files[i].first;
    }
    const uint32_t cdSize = uint32_t(z.size()) - cdOff;
    z += "PK\x05\x06"; put16(z, 0); put16(z, 0);
    put16(z, uint16_t(files.size())); put16(z, uint16_t(files.size()));
    put32(z, cdSize); put32(z, cdOff); put16(z, 0);
    return z;
}

int main(int argc, char** argv) {
    const fs::path tmp = fs::temp_directory_path() / "sf_apply_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "game");
    const fs::path game = tmp / "game";

    std::printf("A real fix archive into a game folder\n");
    if (argc > 1 && fs::exists(argv[1])) {
        // The file the fix is going to replace, as the game shipped it.
        { std::ofstream f(game / "steam_api64.dll"); f << "the original"; }

        const std::string r = LuaFlipperDownload::Apply(argv[1], game.string());
        std::printf("  %s\n", r.c_str());
        check(has(r, "\"ok\":true"), "applied");
        check(fs::exists(game / "steam_api64.dll"), "root file written");
        check(fs::exists(game / "steam_settings" / "steam_appid.txt"),
              "subdirectory recreated");
        check(fs::exists(game / "steam_api64.dll.sfbak"), "original backed up");
        check(slurp(game / "steam_api64.dll.sfbak") == "the original",
              "the backup is the file the game shipped");
        check(slurp(game / "steam_api64.dll") != "the original",
              "the fix overwrote it");
        check(has(r, "\"backed\":1"), "one backup reported");

        // Applying a second time must not overwrite the backup with the fix.
        LuaFlipperDownload::Apply(argv[1], game.string());
        check(slurp(game / "steam_api64.dll.sfbak") == "the original",
              "a second apply leaves the first backup alone");
    } else {
        std::printf("  (no archive given, skipped)\n");
    }

    std::printf("A name that climbs out of the game folder\n");
    {
        fs::create_directories(tmp / "hostile");
        const fs::path zip = tmp / "hostile.zip";
        { std::ofstream f(zip, std::ios::binary);
          const std::string z = makeZip({{"../escaped.txt", "no"},
                                         {"/etc/passwd", "no"},
                                         {"fine.txt", "yes"}});
          f.write(z.data(), z.size()); }
        const std::string r = LuaFlipperDownload::Apply(zip.string(),
                                                        (tmp / "hostile").string());
        std::printf("  %s\n", r.c_str());
        check(!fs::exists(tmp / "escaped.txt"), "no file outside the folder");
        check(fs::exists(tmp / "hostile" / "fine.txt"), "the safe entry landed");
        check(has(r, "escapes the game folder"), "the refusal is reported");
        check(has(r, "\"applied\":1"), "only one entry counted");
    }

    std::printf("A password-protected archive\n");
    {
        fs::create_directories(tmp / "locked");
        const fs::path zip = tmp / "locked.zip";
        std::string z = makeZip({{"a.dll", "x"}});
        // Flip the encryption bit in both headers.
        for (size_t i = 0; i + 1 < z.size(); i++)
            if (!z.compare(i, 4, "PK\x03\x04")) z[i + 6] |= 1;
            else if (!z.compare(i, 4, "PK\x01\x02")) z[i + 8] |= 1;
        { std::ofstream f(zip, std::ios::binary); f.write(z.data(), z.size()); }
        const std::string r = LuaFlipperDownload::Apply(zip.string(),
                                                        (tmp / "locked").string());
        std::printf("  %s\n", r.c_str());
        check(has(r, "password protected"), "named as a password, not a corruption");
        check(!fs::exists(tmp / "locked" / "a.dll"), "nothing written");
    }

    std::printf("Finding a game folder from an appid\n");
    {
        // Against the real Steam on this machine, so the shape of
        // libraryfolders.vdf is the one Steam actually writes.
        const fs::path steam = fs::path(getenv("HOME")) / ".local/share/Steam";

        // Every app manifest in every library, so a second drive is covered
        // whenever the machine running this has one.
        std::vector<fs::path> libs{ steam };
        {
            std::ifstream f(steam / "steamapps" / "libraryfolders.vdf");
            std::string line;
            while (std::getline(f, line)) {
                const size_t k = line.find("\"path\"");
                if (k == std::string::npos) continue;
                const size_t q = line.find('"', k + 6);
                const size_t e = line.find('"', q + 1);
                if (q != std::string::npos && e != std::string::npos)
                    libs.push_back(line.substr(q + 1, e - q - 1));
            }
        }

        int found = 0, missed = 0;
        std::error_code ec;
        for (const fs::path& lib : libs) {
            for (auto& e : fs::directory_iterator(lib / "steamapps", ec)) {
                const std::string n = e.path().filename().string();
                if (n.compare(0, 12, "appmanifest_") != 0) continue;
                const std::string id = n.substr(12, n.size() - 16);
                if (LuaFlipperDownload::GameDir(id, steam.string()).empty()) {
                    missed++;
                    std::printf("    %s -> nothing\n", id.c_str());
                } else {
                    found++;
                }
            }
        }
        std::printf("    %zu libraries, %d apps resolved, %d unresolved\n",
                    libs.size(), found, missed);
        check(found > 0, "installed apps resolve to a folder");
        check(missed == 0, "every installed app resolves");
        check(LuaFlipperDownload::GameDir("999999999", steam.string()).empty(),
              "an app that is not installed resolves to nothing");
    }

    std::printf("A folder that is not there\n");
    {
        const std::string r = LuaFlipperDownload::Apply("/nonexistent.zip",
                                                        "/nonexistent/dir");
        std::printf("  %s\n", r.c_str());
        check(has(r, "\"error\""), "refused");
    }

    fs::remove_all(tmp);
    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
