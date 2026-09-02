#include "Utils/Tokeer/TokeerBridge.h"
#include "Utils/Logging/Log.h"
#include "SFPlatform/include/DynamicLibrary.h"

#ifdef _WIN32
#include <windows.h>

#include <string>

extern "C" __declspec(dllexport) void CALLBACK TokeerUri(HWND, HINSTANCE, LPSTR lpszCmdLine, int)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&TokeerUri), &self);
    Log::Init(reinterpret_cast<SFPlatform::DynamicLibrary::ModuleHandle>(self));

    TokeerBridge::HandleUri(lpszCmdLine ? std::string(lpszCmdLine) : std::string());
}
#endif
