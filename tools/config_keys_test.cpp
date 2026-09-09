// Runnable check for LuaFlipperDownload::ConfigDepotKeys, the scan that decides
// whether the Unlocker page offers to write depot keys and close Steam.
//
// Linked against the object the build already produced, so it exercises the
// shipping code rather than a copy of it.
//
//   g++ -m32 -std=c++20 -I src tools/config_keys_test.cpp \
//       build/32/CMakeFiles/SteamFlipper.dir/Utils/LuaFlipperUI/LuaFlipperDownload.cpp.o \
//       -lz -o /tmp/config_keys_test
//   /tmp/config_keys_test
//
// Worth having because both ways of being wrong are silent. Miss a depot that
// already has its key and the page offers to close somebody's client to write
// nothing; count one that has no key and a real gap is reported as closed,
// leaving downloads failing as "content still encrypted" with nothing on
// screen saying why.
//
// The last case runs against this machine's own config.vdf, because the format
// is Valve's and the only trustworthy sample of it is a real one.
#include "Utils/LuaFlipperUI/LuaFlipperDownload.h"
#include "SFPlatform/include/Http.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// The three things the object needs that live in other translation units.
// The scan touches none of them; they only have to resolve.
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

// Writes a config.vdf under a throwaway Steam root and returns the root.
static fs::path steamWith(const fs::path& tmp, const std::string& name,
                          const std::string& body) {
    const fs::path root = tmp / name;
    fs::create_directories(root / "config");
    std::ofstream(root / "config" / "config.vdf") << body;
    return root;
}

static std::vector<uint32_t> sorted(std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "sf_config_keys_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::printf("A depot with a key, and one without\n");
    {
        // The second block is the case that matters: a depot Steam knows about
        // that carries no key. Counting it would hide a real gap.
        const auto root = steamWith(tmp, "mixed", R"(
"InstallConfigStore"
{
    "Software" { "Valve" { "Steam" {
        "depots"
        {
            "1091501"  { "DecryptionKey"  "aabb" }
            "2222222"  { "SizeOnDisk"  "1234" }
        }
    } } }
}
)");
        const auto got = sorted(LuaFlipperDownload::ConfigDepotKeys(root.string()));
        check(got.size() == 1 && got[0] == 1091501u,
              "only the depot that has a key is reported");
    }

    std::printf("Several keys\n");
    {
        const auto root = steamWith(tmp, "many", R"(
"depots"
{
    "10"  { "DecryptionKey"  "00" }
    "20"  { "DecryptionKey"  "11" }
    "30"  { "DecryptionKey"  "22" }
}
)");
        const auto got = sorted(LuaFlipperDownload::ConfigDepotKeys(root.string()));
        check(got == std::vector<uint32_t>({10u, 20u, 30u}), "all three are found");
    }

    std::printf("Nothing to find\n");
    {
        const auto root = steamWith(tmp, "empty", "\"InstallConfigStore\"\n{\n}\n");
        check(LuaFlipperDownload::ConfigDepotKeys(root.string()).empty(),
              "a config with no depots block reports none");

        // Not an error: a Steam that has never been logged into has no such
        // file, and claiming every key is present would suppress the offer.
        check(LuaFlipperDownload::ConfigDepotKeys((tmp / "nope").string()).empty(),
              "a missing config.vdf reports none rather than throwing");
    }

    std::printf("Input that must not throw\n");
    {
        // The scan runs on every page open, inside the UI thread, over a file
        // this process does not write. It has to survive whatever is in it.
        const auto root = steamWith(tmp, "hostile", R"(
"depots"
{
    "99999999999999999999"  { "DecryptionKey"  "00" }
    "notanumber"            { "DecryptionKey"  "11" }
    ""                      { "DecryptionKey"  "22" }
    "DecryptionKey"
    "77"  { "DecryptionKey"  "33" }
}
)");
        const auto got = sorted(LuaFlipperDownload::ConfigDepotKeys(root.string()));
        check(std::find(got.begin(), got.end(), 77u) != got.end(),
              "the one valid depot is still found among the junk");
        check(std::find(got.begin(), got.end(), 0u) == got.end(),
              "an unparseable id is skipped, not recorded as depot 0");
    }

    std::printf("This machine's own config.vdf\n");
    {
        const char* home = std::getenv("HOME");
        const fs::path root = fs::path(home ? home : "") / ".local/share/Steam";
        if (!fs::exists(root / "config" / "config.vdf")) {
            std::printf("  skip no Steam install here\n");
        } else {
            const auto got = LuaFlipperDownload::ConfigDepotKeys(root.string());
            check(!got.empty(), "a real config.vdf yields depots");
            check(std::find(got.begin(), got.end(), 0u) == got.end(),
                  "no depot 0, which is what a mis-parse would produce");
            std::printf("  note %zu depots carry a key here; compare with\n"
                        "       tools/sync_depot_keys.py --dry-run\n", got.size());
        }
    }

    fs::remove_all(tmp);
    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
