#include "include/Encoding.h"

#include "include/Log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cwchar>
#include <cstdlib>
#endif

#include <limits>

namespace SFPlatform::Encoding {
namespace {

bool FitsInt(size_t size, const char* operation) {
    if (size <= static_cast<size_t>((std::numeric_limits<int>::max)())) return true;
    SFP_LOG_DEBUG("{}: input is too large ({} bytes/chars)", operation, size);
    return false;
}

} // namespace

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (!FitsInt(value.size(), "WideToUtf8")) return {};

#ifdef _WIN32
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        SFP_LOG_DEBUG("WideCharToMultiByte(size query) failed (error={})", GetLastError());
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                            result.data(), size, nullptr, nullptr);
    if (written != size) {
        SFP_LOG_DEBUG("WideCharToMultiByte(convert) failed (written={}, expected={}, error={})",
                       written, size, GetLastError());
        return {};
    }
    return result;
#else
    std::mbstate_t state{};
    const wchar_t* src = value.data();
    size_t inLen = value.size();
    // Temporary null-terminated buffer if needed
    std::wstring nullTerminated(value);
    const wchar_t* ptr = nullTerminated.c_str();
    size_t req = std::wcsrtombs(nullptr, &ptr, 0, &state);
    if (req == static_cast<size_t>(-1)) {
        return {};
    }
    std::string result(req, '\0');
    ptr = nullTerminated.c_str();
    std::wcsrtombs(result.data(), &ptr, req, &state);
    return result;
#endif
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    if (!FitsInt(value.size(), "Utf8ToWide")) return {};

#ifdef _WIN32
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        SFP_LOG_DEBUG("MultiByteToWideChar(size query) failed (error={})", GetLastError());
        return {};
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    if (written != size) {
        SFP_LOG_DEBUG("MultiByteToWideChar(convert) failed (written={}, expected={}, error={})",
                       written, size, GetLastError());
        return {};
    }
    return result;
#else
    std::mbstate_t state{};
    std::string nullTerminated(value);
    const char* ptr = nullTerminated.c_str();
    size_t req = std::mbsrtowcs(nullptr, &ptr, 0, &state);
    if (req == static_cast<size_t>(-1)) {
        return {};
    }
    std::wstring result(req, L'\0');
    ptr = nullTerminated.c_str();
    std::mbsrtowcs(result.data(), &ptr, req, &state);
    return result;
#endif
}

} // namespace SFPlatform::Encoding
