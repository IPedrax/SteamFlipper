#include "include/Detour.h"

#include "include/Log.h"

#include <funchook.h>
#include <algorithm>
#include <mutex>
#include <unordered_set>
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

// Handles whose hooks have already been uninstalled but whose trampolines are
// still allocated, because entries referencing them remain in g_activeHooks.
// funchook_destroy runs once the last of those is gone.
std::unordered_set<funchook_t*> g_uninstalled;

} // namespace

bool BeginTransaction() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_funchook) {
        // An abandoned transaction: BeginTransaction called twice without a
        // Commit. Anything prepared on the old handle still points into
        // trampolines this destroy unmaps, so roll those back first.
        for (auto it = g_activeHooks.begin(); it != g_activeHooks.end(); ) {
            if (it->hookHandle == g_funchook) {
                if (it->target) *it->target = it->original;
                it = g_activeHooks.erase(it);
            } else {
                ++it;
            }
        }
        g_uninstalled.erase(g_funchook);
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
        g_uninstalled.erase(g_funchook);
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

    // funchook uninstalls per *handle*, not per hook, and every hook prepared
    // in one transaction shares that handle. So the first Detach touching a
    // handle takes all of its hooks off at once. That is what makes it safe for
    // the caller to null its original-function pointer straight after: no
    // detour on this handle is routing any more, so nothing can call through a
    // pointer that is about to become null.
    //
    // Previously this returned false whenever the hook had siblings, so
    // uninstalling any multi-hook module simply failed. Before that it returned
    // true without uninstalling anything, which left the detour live while the
    // caller nulled the original it depended on.
    if (handle && g_uninstalled.find(handle) == g_uninstalled.end()) {
        funchook_uninstall(handle, 0);
        g_uninstalled.insert(handle);

        // Put every hook on this handle back on the real function while its
        // trampoline is still mapped.
        for (auto& hook : g_activeHooks) {
            if (hook.hookHandle == handle && hook.target)
                *hook.target = hook.original;
        }
    }

    g_activeHooks.erase(it);

    // Destroy only once the last entry for this handle is gone. Doing it
    // earlier would unmap trampolines that surviving entries still point at.
    const bool stillReferenced =
        std::any_of(g_activeHooks.begin(), g_activeHooks.end(),
                    [&](const ActiveHook& hook) { return hook.hookHandle == handle; });
    if (handle && !stillReferenced) {
        funchook_destroy(handle);
        g_uninstalled.erase(handle);
    }
    return true;
}

} // namespace SFPlatform::Detour
