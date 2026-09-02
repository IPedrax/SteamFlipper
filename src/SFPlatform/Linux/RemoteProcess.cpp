#include "include/RemoteProcess.h"

#include "include/Log.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <string>

namespace SFPlatform::RemoteProcess {

const char* ToString(Architecture architecture) {
    switch (architecture) {
    case Architecture::X86: return "x86";
    case Architecture::X64: return "x64";
    default: return "Unknown";
    }
}

const char* ToString(InjectStatus status) {
    switch (status) {
    case InjectStatus::Ok: return "Ok";
    case InjectStatus::OpenProcessFailed: return "OpenProcessFailed";
    case InjectStatus::UnknownArchitecture: return "UnknownArchitecture";
    case InjectStatus::AllocFailed: return "AllocFailed";
    case InjectStatus::WriteFailed: return "WriteFailed";
    case InjectStatus::ResolveLoadLibraryFailed: return "ResolveLoadLibraryFailed";
    case InjectStatus::CreateThreadFailed: return "CreateThreadFailed";
    case InjectStatus::WaitFailed: return "WaitFailed";
    case InjectStatus::RemoteLoadFailed: return "RemoteLoadFailed";
    }
    return "Unknown";
}

Architecture GetArchitecture(uint32_t pid) {
    std::string exePath = "/proc/" + std::to_string(pid) + "/exe";
    int fd = open(exePath.c_str(), O_RDONLY);
    if (fd < 0) return Architecture::Unknown;

    unsigned char e_ident[EI_NIDENT];
    ssize_t bytesRead = read(fd, e_ident, sizeof(e_ident));
    close(fd);

    if (bytesRead < static_cast<ssize_t>(sizeof(e_ident))) return Architecture::Unknown;
    if (e_ident[EI_MAG0] != ELFMAG0 || e_ident[EI_MAG1] != ELFMAG1 ||
        e_ident[EI_MAG2] != ELFMAG2 || e_ident[EI_MAG3] != ELFMAG3) {
        return Architecture::Unknown;
    }

    if (e_ident[EI_CLASS] == ELFCLASS64) return Architecture::X64;
    if (e_ident[EI_CLASS] == ELFCLASS32) return Architecture::X86;
    return Architecture::Unknown;
}

InjectStatus InjectLibrary(uint32_t pid, const std::filesystem::path& libraryPath) {
    // Not implemented on Linux. Remote injection here needs ptrace + dlopen
    // shellcode (and ptrace_scope permitting it); the client-side hooks do not
    // use this path, and game processes are reached via LD_PRELOAD at launch.
    //
    // Returning Ok made Injection::Apply report every configured entry as
    // successfully injected while nothing happened, so a user's inject_dlls
    // list looked like it worked. Report the failure instead.
    SFP_LOG_WARN("RemoteProcess::InjectLibrary: not supported on Linux "
                  "(pid={} library='{}'); no injection performed",
                  pid, libraryPath.string());
    return InjectStatus::RemoteLoadFailed;
}

} // namespace SFPlatform::RemoteProcess
