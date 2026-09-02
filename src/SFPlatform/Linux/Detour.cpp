#include "include/Detour.h"

#include "include/Log.h"

#include <funchook.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace SFPlatform::Detour {
namespace {

std::mutex g_mutex;
funchook_t* g_funchook = nullptr;

struct ActiveHook {
    funchook_t* hookHandle = nullptr;
    void** target = nullptr;
    void* detour = nullptr;
    // Value of *target before funchook_prepare redirected it at a trampoline.
    // Needed to put callers back on the real function when a transaction is
    // rolled back or a hook is torn down -- the trampoline is unmapped by
    // funchook_destroy, so leaving *target pointing into it is a dangling jump.
    void* original = nullptr;
};
std::vector<ActiveHook> g_activeHooks;

} // namespace

bool BeginTransaction() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_funchook) {
        funchook_destroy(g_funchook);
        g_funchook = nullptr;
    }
    g_funchook = funchook_create();
    if (!g_funchook) {
        SFP_LOG_WARN("BeginTransaction: funchook_create failed");
        return false;
    }
    return true;
}

bool CommitTransaction() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_funchook) {
        SFP_LOG_WARN("CommitTransaction: no transaction in progress");
        return false;
    }
    int rv = funchook_install(g_funchook, 0);
    if (rv != FUNCHOOK_ERROR_SUCCESS) {
        SFP_LOG_WARN("CommitTransaction: funchook_install failed (error={}: {})",
                      rv, funchook_error_message(g_funchook));
        // Roll back before destroying. funchook_prepare already pointed every
        // *target at a trampoline inside this handle, and funchook_destroy
        // unmaps those pages -- so without this, the next call through any
        // o##name from this transaction jumps into freed memory. Dropping the
        // entries also stops a later Detach from touching the dead handle.
        for (auto it = g_activeHooks.begin(); it != g_activeHooks.end(); ) {
            if (it->hookHandle == g_funchook) {
                if (it->target) *it->target = it->original;
                it = g_activeHooks.erase(it);
            } else {
                ++it;
            }
        }
        funchook_destroy(g_funchook);
        g_funchook = nullptr;
        return false;
    }
    // Transaction successfully installed; reset transaction handle without destroying installed hooks
    g_funchook = nullptr;
    return true;
}

bool Attach(void** target, void* detour) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_funchook) {
        // If not in an explicit transaction, create a single-op hook
        g_funchook = funchook_create();
        if (!g_funchook) return false;
    }
    if (!target || !*target || !detour) {
        SFP_LOG_WARN("Attach: invalid target or detour pointer");
        return false;
    }

    // Capture before prepare: funchook_prepare overwrites *target in place.
    void* original = *target;

    int rv = funchook_prepare(g_funchook, target, detour);
    if (rv != FUNCHOOK_ERROR_SUCCESS) {
        SFP_LOG_WARN("Attach: funchook_prepare failed (error={}: {})",
                      rv, funchook_error_message(g_funchook));
        return false;
    }

    g_activeHooks.push_back({g_funchook, target, detour, original});
    return true;
}

bool Detach(void** target, void* detour) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = std::find_if(g_activeHooks.begin(), g_activeHooks.end(),
                           [&](const ActiveHook& hook) {
                               return hook.target == target && hook.detour == detour;
                           });
    if (it == g_activeHooks.end()) {
        return false;
    }

    funchook_t* handle = it->hookHandle;

    // funchook installs and uninstalls per *handle*, not per hook, and every
    // hook prepared inside one transaction shares that handle. There is no way
    // to remove just one.
    const bool hasSiblings =
        std::any_of(g_activeHooks.begin(), g_activeHooks.end(),
                    [&](const ActiveHook& hook) {
                        return &hook != &(*it) && hook.hookHandle == handle;
                    });

    if (hasSiblings) {
        // Previously this erased the entry and returned true, so the caller
        // (UNINSTALL_HOOK) nulled o##name while the detour was still installed
        // and still routing through it. Report the failure instead: the hook
        // stays live, and its trampoline stays valid, which is the truth.
        SFP_LOG_WARN("Detach: hook shares a funchook handle with others; "
                      "funchook cannot uninstall one in isolation, leaving it installed");
        return false;
    }

    // Last hook on this handle: tear it down, then put the caller back on the
    // real function before the trampoline is unmapped.
    if (handle) {
        funchook_uninstall(handle, 0);
        funchook_destroy(handle);
    }
    if (it->target) *it->target = it->original;
    g_activeHooks.erase(it);
    return true;
}

} // namespace SFPlatform::Detour
