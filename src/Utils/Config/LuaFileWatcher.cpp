#include <cstdlib>
#include "Hook/Hooks_Package.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/CloudSaves/CloudSaves.h"
#include "Utils/Logging/Log.h"
#include "SFPlatform/include/DirectoryWatch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <thread>
#include <unordered_map>

namespace LuaFileWatcher {
namespace {

enum class ChangeAction {
    Added,
    Modified,
    Removed,
};

struct FileChange {
    std::string path;
    ChangeAction action = ChangeAction::Modified;
};

std::atomic<bool> g_running{false};
std::thread g_watcherThread;
std::vector<std::string> g_watchDirs;

constexpr uint32_t kDebounceMs = 500;

bool IsLuaFile(const std::string& path) {
    if (path.size() < 4) return false;
    return std::equal(path.end() - 4, path.end(), ".lua", [](char lhs, char rhs) {
        return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
    });
}

ChangeAction FromPlatformAction(SFPlatform::DirectoryWatch::ChangeAction action) {
    switch (action) {
    case SFPlatform::DirectoryWatch::ChangeAction::Added:
    case SFPlatform::DirectoryWatch::ChangeAction::RenamedNewName:
        return ChangeAction::Added;
    case SFPlatform::DirectoryWatch::ChangeAction::Removed:
    case SFPlatform::DirectoryWatch::ChangeAction::RenamedOldName:
        return ChangeAction::Removed;
    case SFPlatform::DirectoryWatch::ChangeAction::Modified:
        return ChangeAction::Modified;
    }
    return ChangeAction::Modified;
}

const char* ToString(ChangeAction action) {
    switch (action) {
    case ChangeAction::Added: return "added";
    case ChangeAction::Modified: return "modified";
    case ChangeAction::Removed: return "removed";
    }
    return "modified";
}

void MergeChanges(
    std::unordered_map<std::string, ChangeAction>& accumulated,
    std::vector<std::string>& order,
    const std::vector<FileChange>& newChanges) {
    for (const auto& ch : newChanges) {
        if (!accumulated.contains(ch.path)) {
            order.push_back(ch.path);
        }
        accumulated[ch.path] = ch.action;
    }
}

std::vector<FileChange> FlattenChanges(
    const std::unordered_map<std::string, ChangeAction>& accumulated,
    const std::vector<std::string>& order) {
    std::vector<FileChange> changes;
    changes.reserve(order.size());
    for (const auto& path : order) {
        changes.push_back({path, accumulated.at(path)});
    }
    return changes;
}

std::vector<FileChange> ToFileChanges(
    const std::string& dir,
    const std::vector<SFPlatform::DirectoryWatch::Change>& changes) {
    std::vector<FileChange> result;
    result.reserve(changes.size());
    for (const auto& change : changes) {
        if (change.relativePath.empty()) continue;
        // Join with the platform separator: a hardcoded '\' produced paths like
        // "…/stplug-in\381210.lua" on Linux, which ParseFile could not open, so
        // every watched .lua silently failed to load.
        result.push_back({(std::filesystem::path(dir) / change.relativePath).string(),
                          FromPlatformAction(change.action)});
    }
    return result;
}

void ProcessChanges(const std::vector<FileChange>& changes) {
    std::vector<FileChange> luaChanges;
    for (const auto& change : changes) {
        if (IsLuaFile(change.path)) {
            luaChanges.push_back(change);
        }
    }
    if (luaChanges.empty()) return;

    LOG_PACKAGE_DEBUG("Processing {} Lua file change(s)", luaChanges.size());
    for (const auto& change : luaChanges) {
        LOG_PACKAGE_TRACE("Lua file {}: {}", ToString(change.action), change.path);
        if (change.action == ChangeAction::Removed) {
            LuaConfig::UnloadFile(change.path);
        } else {
            LuaConfig::ParseFile(change.path);
        }
    }

    Hooks_Package::NotifyLicenseChanged();
    CloudRedirectHost::SyncAppSet();
    CloudSaves::SyncAppSet();
    LOG_PACKAGE_DEBUG("Lua refresh completed");
}

void WatcherThread() {
    const size_t numDirs = g_watchDirs.size();
    std::vector<SFPlatform::DirectoryWatch::Watch> watches(numDirs);
    std::vector<SFPlatform::DirectoryWatch::Watch*> watchPtrs(numDirs, nullptr);

    for (size_t i = 0; i < numDirs; ++i) {
        if (!watches[i].Open(g_watchDirs[i], 65536)) {
            LOG_PACKAGE_WARN("Failed to open Lua watch directory: {}", g_watchDirs[i]);
            continue;
        }
        if (!watches[i].IssueRead()) {
            watches[i].Cancel();
            continue;
        }

        watchPtrs[i] = &watches[i];
        LOG_PACKAGE_DEBUG("Watching Lua directory: {}", g_watchDirs[i]);
    }

    bool allFailed = true;
    for (auto* watch : watchPtrs) {
        if (watch && watch->IsOpen()) {
            allFailed = false;
            break;
        }
    }
    if (allFailed) {
        LOG_PACKAGE_WARN("No Lua watch directories could be opened");
        return;
    }

    auto drainEvent = [&](size_t idx,
                          std::unordered_map<std::string, ChangeAction>& accumulated,
                          std::vector<std::string>& order) {
        auto* watch = idx < watchPtrs.size() ? watchPtrs[idx] : nullptr;
        if (!watch || !watch->IsOpen()) return;

        MergeChanges(accumulated, order, ToFileChanges(g_watchDirs[idx], watch->Drain()));
        watch->IssueRead();
    };

    while (g_running) {
        auto waitResult = SFPlatform::DirectoryWatch::WaitAny(watchPtrs, 1000);

        if (!g_running) break;
        if (waitResult.status == SFPlatform::DirectoryWatch::WaitStatus::Timeout) continue;
        if (waitResult.status != SFPlatform::DirectoryWatch::WaitStatus::Signaled ||
            waitResult.index >= numDirs) {
            continue;
        }

        std::unordered_map<std::string, ChangeAction> accumulated;
        std::vector<std::string> order;

        drainEvent(waitResult.index, accumulated, order);

        while (g_running) {
            auto debounceResult = SFPlatform::DirectoryWatch::WaitAny(watchPtrs, kDebounceMs);
            if (!g_running) break;
            if (debounceResult.status == SFPlatform::DirectoryWatch::WaitStatus::Timeout) break;
            if (debounceResult.status != SFPlatform::DirectoryWatch::WaitStatus::Signaled ||
                debounceResult.index >= numDirs) {
                break;
            }
            drainEvent(debounceResult.index, accumulated, order);
        }

        if (!order.empty()) {
            ProcessChanges(FlattenChanges(accumulated, order));
        }
    }

    for (auto& watch : watches) {
        watch.Cancel();
    }
    LOG_PACKAGE_DEBUG("Lua watcher stopped");
}

} // namespace

void Start(const std::vector<std::string>& directories) {
    if (g_running.exchange(true)) {
        LOG_PACKAGE_WARN("Lua watcher already running");
        return;
    }

    /*
     * Join before the static std::thread is destroyed, not after.
     *
     * ~thread() on a joinable thread calls std::terminate, and the destructor
     * of a namespace-scope thread object is registered at static-init time --
     * before this function ever runs. Exit handlers run in reverse
     * registration order, so an atexit registered here runs FIRST, and Stop()
     * gets to join while the object is still alive.
     *
     * The module's own unload hook calls Stop() too, but that lives in
     * .fini_array, which glibc runs after the __cxa_atexit list has already
     * destroyed this object. That ordering is why every Steam exit ended in
     * abort with a core dump: 114 of them on the machine this was found on,
     * all after Steam had finished its own shutdown, which is why nothing
     * looked wrong.
     */
    static bool once = (std::atexit(Stop), true);
    (void)once;

    g_watchDirs = directories;
    g_watcherThread = std::thread(WatcherThread);
}

void Stop() {
    if (!g_running) return;
    g_running = false;
    if (g_watcherThread.joinable()) {
        g_watcherThread.join();
    }
}

} // namespace LuaFileWatcher
