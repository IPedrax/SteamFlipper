#include "include/SteamCredentialStore.h"

#include "include/Encoding.h"
#include "include/Log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace SFPlatform::SteamCredentialStore {
namespace {

std::filesystem::path GetSteamConfigDir() {
    const char* home = std::getenv("HOME");
    if (!home) return "/tmp";

    std::filesystem::path p1 = std::filesystem::path(home) / ".local/share/Steam/config";
    if (std::filesystem::exists(p1)) return p1;

    std::filesystem::path p2 = std::filesystem::path(home) / ".steam/steam/config";
    if (std::filesystem::exists(p2)) return p2;

    return p1;
}

std::filesystem::path GetAppStoreDir(uint32_t appId) {
    std::filesystem::path store = GetSteamConfigDir() / "sf_credentials" / std::to_string(appId);
    std::error_code ec;
    std::filesystem::create_directories(store, ec);
    return store;
}

Status ReadBinary(const std::filesystem::path& file, std::vector<uint8_t>& out) {
    std::ifstream is(file, std::ios::binary | std::ios::ate);
    if (!is.is_open()) return Status::NotFound;

    std::streamsize size = is.tellg();
    if (size <= 0) return Status::NotFound;

    is.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (is.read(reinterpret_cast<char*>(out.data()), size)) {
        return Status::Ok;
    }
    return Status::Failed;
}

Status WriteBinary(const std::filesystem::path& file, const std::vector<uint8_t>& data) {
    std::ofstream os(file, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) return Status::Failed;

    if (os.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        return Status::Ok;
    }
    return Status::Failed;
}

} // namespace

const char* ToString(Status status) {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::NotFound: return "NotFound";
    case Status::Unsupported: return "Unsupported";
    case Status::Failed: return "Failed";
    }
    return "Unknown";
}

Status GetAppTicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    return ReadBinary(GetAppStoreDir(appId) / "AppTicket.bin", ticket);
}

Status WriteAppTicket(uint32_t appId, const std::vector<uint8_t>& data) {
    return WriteBinary(GetAppStoreDir(appId) / "AppTicket.bin", data);
}

Status GetETicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    return ReadBinary(GetAppStoreDir(appId) / "ETicket.bin", ticket);
}

Status WriteETicket(uint32_t appId, const std::vector<uint8_t>& data) {
    return WriteBinary(GetAppStoreDir(appId) / "ETicket.bin", data);
}

Status GetSteamId(uint32_t appId, uint64_t& steamId) {
    std::ifstream is(GetAppStoreDir(appId) / "SteamID.txt");
    if (!is.is_open()) return Status::NotFound;
    is >> steamId;
    return is.good() || is.eof() ? Status::Ok : Status::Failed;
}

Status WriteSteamId(uint32_t appId, uint64_t steamId) {
    std::ofstream os(GetAppStoreDir(appId) / "SteamID.txt");
    if (!os.is_open()) return Status::Failed;
    os << steamId;
    return Status::Ok;
}

Status GetActiveUser(uint32_t& accountId, std::wstring& universe) {
    // Attempt to read ~/.steam/steam/config/loginusers.vdf
    std::filesystem::path loginUsers = GetSteamConfigDir() / "loginusers.vdf";
    std::ifstream is(loginUsers);
    if (!is.is_open()) {
        accountId = 0;
        universe = L"Public";
        return Status::NotFound;
    }

    std::string line;
    uint64_t steam64 = 0;
    while (std::getline(is, line)) {
        size_t quote1 = line.find('"');
        if (quote1 != std::string::npos) {
            size_t quote2 = line.find('"', quote1 + 1);
            if (quote2 != std::string::npos) {
                std::string key = line.substr(quote1 + 1, quote2 - quote1 - 1);
                if (key.size() == 17 && key.starts_with("7656119")) {
                    try {
                        steam64 = std::stoull(key);
                        break;
                    } catch (...) {}
                }
            }
        }
    }

    if (steam64 != 0) {
        accountId = static_cast<uint32_t>(steam64 & 0xFFFFFFFFu);
        universe = L"Public";
        return Status::Ok;
    }

    return Status::NotFound;
}

} // namespace SFPlatform::SteamCredentialStore
