#include "include/Process.h"

#include "include/Encoding.h"
#include "include/Log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace SFPlatform::Process {

std::optional<uint64_t> GetCreationTime(uint32_t pid) {
    std::string statPath = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream statFile(statPath);
    if (!statFile.is_open()) return std::nullopt;

    std::string content;
    std::getline(statFile, content);
    if (content.empty()) return std::nullopt;

    // Field 22 (starttime) in /proc/[pid]/stat
    size_t lastParen = content.find_last_of(')');
    if (lastParen == std::string::npos) return std::nullopt;

    std::istringstream iss(content.substr(lastParen + 1));
    std::string token;
    // Skip to field 22 (we've parsed through field 2, so skip 19 fields)
    for (int i = 0; i < 19; ++i) {
        if (!(iss >> token)) return std::nullopt;
    }
    if (!(iss >> token)) return std::nullopt;

    try {
        return std::stoull(token);
    } catch (...) {
        return std::nullopt;
    }
}

std::string FormatCreationTime(uint64_t fileTime) {
    return std::to_string(fileTime);
}

std::optional<std::string> GetImagePath(uint32_t pid) {
    std::string exeLink = "/proc/" + std::to_string(pid) + "/exe";
    char buffer[PATH_MAX];
    ssize_t len = readlink(exeLink.c_str(), buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        return std::string(buffer);
    }
    return std::nullopt;
}

std::optional<std::string> GetEnvironmentVariableValue(uint32_t pid, std::wstring_view name) {
    const std::string needle = Encoding::WideToUtf8(name) + "=";
    if (needle.size() <= 1) return std::nullopt;

    std::string envPath = "/proc/" + std::to_string(pid) + "/environ";
    std::ifstream envFile(envPath, std::ios::binary);
    if (!envFile.is_open()) return std::nullopt;

    std::string token;
    while (std::getline(envFile, token, '\0')) {
        if (token.rfind(needle, 0) == 0) {
            return token.substr(needle.size());
        }
    }
    return std::nullopt;
}

std::optional<std::string> GetProcessCommandLine(uint32_t pid) {
    std::string cmdPath = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream cmdFile(cmdPath, std::ios::binary);
    if (!cmdFile.is_open()) return std::nullopt;

    std::string cmdLine;
    std::string arg;
    while (std::getline(cmdFile, arg, '\0')) {
        if (!cmdLine.empty()) cmdLine += " ";
        cmdLine += arg;
    }
    return cmdLine.empty() ? std::nullopt : std::optional<std::string>(cmdLine);
}

std::vector<ModuleInfo> EnumerateModules(uint32_t pid) {
    std::vector<ModuleInfo> modules;
    std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream mapsFile(mapsPath);
    if (!mapsFile.is_open()) return modules;

    std::string line;
    while (std::getline(mapsFile, line)) {
        std::istringstream iss(line);
        std::string range, perms, offset, dev, inode, pathname;
        if (iss >> range >> perms >> offset >> dev >> inode >> pathname) {
            if (!pathname.empty() && pathname[0] == '/') {
                bool executable = (perms.find('x') != std::string::npos);
                modules.push_back({
                    pathname,
                    std::filesystem::path(pathname),
                    0,
                    executable
                });
            }
        }
    }
    return modules;
}

bool IsSystemModulePath(const std::string& path) {
    return path.starts_with("/usr/lib") ||
           path.starts_with("/lib") ||
           path.starts_with("/usr/lib64") ||
           path.starts_with("/lib64");
}

bool LaunchDetachedHidden(const std::string& commandLine) {
    pid_t pid = fork();
    if (pid < 0) {
        SFP_LOG_WARN("LaunchDetachedHidden: fork failed (errno={})", errno);
        return false;
    }

    if (pid == 0) {
        // In child
        setsid();
        int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }

        /*
         * Everything above the standard three belongs to the process being
         * left behind, and a detached child has no business holding any of it.
         *
         * This is not tidiness. fork() copies every descriptor, exec keeps the
         * ones without CLOEXEC, and the child here goes on to launch a whole
         * process tree -- so a socket the parent was listening on stays bound
         * for as long as any of that tree lives. The auto-update found it the
         * hard way: the helper inherited the module's listening socket on
         * 127.0.0.1:1987 and handed it down through steam.sh to the Steam it
         * restarted, so the port still had a listener, the new module's bind()
         * lost the race for it, and every page in the client hung on a request
         * nothing was left to accept.
         *
         * close_range is one syscall for the whole span; the loop is for
         * kernels before 5.9, where an open file limit in the millions makes
         * this worth bounding rather than walking blind.
         */
#if defined(SYS_close_range)
        if (syscall(SYS_close_range, 3, ~0U, 0) != 0)
#endif
        {
            const long maxFd = sysconf(_SC_OPEN_MAX);
            const int stop = (maxFd > 0 && maxFd < 65536) ? static_cast<int>(maxFd)
                                                          : 65536;
            for (int fd = 3; fd < stop; fd++) close(fd);
        }

        execl("/bin/sh", "sh", "-c", commandLine.c_str(), nullptr);
        _exit(127);
    }

    // In parent: do not wait for child process
    return true;
}

} // namespace SFPlatform::Process
