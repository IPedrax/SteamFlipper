#include "include/Hash.h"

#include "include/Log.h"

#include <openssl/evp.h>

#include <fstream>
#include <vector>

namespace SFPlatform::Hash {
namespace {

std::string HexEncode(const unsigned char* bytes, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += kHex[bytes[i] >> 4];
        result += kHex[bytes[i] & 0xF];
    }
    return result;
}

} // namespace

std::string Sha256OfFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        SFP_LOG_WARN("Sha256OfFile: failed to open '{}'", path.string());
        return {};
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    constexpr size_t kChunk = 65536;
    std::vector<char> buffer(kChunk);
    while (file.read(buffer.data(), kChunk) || file.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(file.gcount())) != 1) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &length) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    EVP_MD_CTX_free(ctx);
    return HexEncode(hash, length);
}

std::string Sha256OfBuffer(const void* data, size_t size) {
    if (!data && size != 0) {
        SFP_LOG_WARN("Sha256OfBuffer: null data with non-zero size ({})", size);
        return {};
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    if (size > 0 && EVP_DigestUpdate(ctx, data, size) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &length) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    EVP_MD_CTX_free(ctx);
    return HexEncode(hash, length);
}

} // namespace SFPlatform::Hash
