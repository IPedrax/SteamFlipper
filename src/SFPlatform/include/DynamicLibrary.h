#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace SFPlatform::DynamicLibrary {

    using ModuleHandle = void*;

    ModuleHandle Load(const std::filesystem::path& path);
    ModuleHandle GetLoaded(std::string_view moduleName);
    void* GetSymbol(ModuleHandle module, const char* symbolName);
    void* GetSymbol(ModuleHandle module, uint16_t ordinal);

    // Load address the module's RVAs are relative to. On Windows an HMODULE is
    // already the base, but dlopen() returns a link_map* that lives on the heap,
    // so adding an RVA to the raw handle lands nowhere near the mapping.
    void* GetModuleBase(ModuleHandle module);
    uint32_t GetLastErrorCode();

    std::string GetCurrentDirectoryPath();
    std::string GetSystemDirectoryPath();
    std::filesystem::path GetModuleDirectory(ModuleHandle module);
    std::filesystem::path GetMainExecutablePath();

} // namespace SFPlatform::DynamicLibrary
