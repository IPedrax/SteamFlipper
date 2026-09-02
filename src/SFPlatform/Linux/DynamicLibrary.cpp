#include "include/DynamicLibrary.h"

#include "include/Log.h"

#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace SFPlatform::DynamicLibrary {

ModuleHandle Load(const std::filesystem::path& path) {
    ModuleHandle module = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!module) {
        SFP_LOG_WARN("dlopen('{}') failed (error={})", path.string(), dlerror());
    }
    return module;
}

ModuleHandle GetLoaded(std::string_view moduleName) {
    // If empty or main program handle requested
    if (moduleName.empty()) {
        return dlopen(nullptr, RTLD_LAZY | RTLD_NOLOAD);
    }
    // Try to obtain handle of already-loaded library
    std::string name(moduleName);
    ModuleHandle module = dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!module) {
        SFP_LOG_DEBUG("GetLoaded('{}') not found (error={})", moduleName, dlerror());
    }
    return module;
}

void* GetSymbol(ModuleHandle module, const char* symbolName) {
    if (!module || !symbolName) return nullptr;
    dlerror(); // Clear existing error
    void* symbol = dlsym(module, symbolName);
    const char* err = dlerror();
    if (err) {
        SFP_LOG_DEBUG("dlsym('{}') failed (error={})", symbolName, err);
        return nullptr;
    }
    return symbol;
}

void* GetSymbol(ModuleHandle /*module*/, uint16_t /*ordinal*/) {
    // Ordinal exports are a PE/Windows-only concept
    return nullptr;
}

void* GetModuleBase(ModuleHandle module) {
    if (!module) return nullptr;
    struct link_map* lm = nullptr;
    if (dlinfo(module, RTLD_DI_LINKMAP, &lm) != 0 || !lm) {
        SFP_LOG_WARN("GetModuleBase: dlinfo failed (error={})", dlerror());
        return nullptr;
    }
    // l_addr is the load bias; the ELF's link-time vaddrs start at 0 for a .so,
    // so base + rva is the runtime address.
    return reinterpret_cast<void*>(lm->l_addr);
}

uint32_t GetLastErrorCode() {
    return 0;
}

std::string GetCurrentDirectoryPath() {
    char buffer[PATH_MAX];
    if (getcwd(buffer, sizeof(buffer))) {
        return std::string(buffer);
    }
    return {};
}

std::string GetSystemDirectoryPath() {
    return "/usr/lib";
}

std::filesystem::path GetModuleDirectory(ModuleHandle module) {
    if (!module) {
        return GetMainExecutablePath().parent_path();
    }
    struct link_map* lm = nullptr;
    if (dlinfo(module, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_name && lm->l_name[0]) {
        return std::filesystem::path(lm->l_name).parent_path();
    }
    return {};
}

std::filesystem::path GetMainExecutablePath() {
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer);
    }
    return {};
}

} // namespace SFPlatform::DynamicLibrary
