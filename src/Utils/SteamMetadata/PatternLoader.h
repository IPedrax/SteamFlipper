#pragma once

#include "SFPlatform/include/DynamicLibrary.h"

#include <string>

namespace PatternLoader {

    // Load metadata before installing hooks for a module.
    bool Load(SFPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component);

    // Resolve by RVA first, then fall back to signature scanning.
    void* FindPattern(SFPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // A structure offset the pattern file recorded beside a function, or 0 when
    // the file does not carry one. Per build, like the address it sits beside:
    // Valve moves both, and a constant in the module is wrong from the first
    // start after it does. Callers must treat 0 as "unknown, do not guess".
    uintptr_t FindFieldOffset(SFPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // Report unresolved functions after all hooks have been installed.
    void ReportMissingFunctions();

} // namespace PatternLoader
