#pragma once

#include <cstdint>

namespace HookUtil {

/*
 * Grow an allocation until it can really hold `need` entries.
 *
 * Valve's CUtlMemory::Grow is advisory: it enlarges by a policy of its own,
 * roughly doubling, rather than by exactly what was asked, so one call from a
 * small base can come back short. Measured in this client as 196 growing to
 * 454 when 131 were requested.
 *
 * The callers that matter treat the result as all-or-nothing: a short
 * allocation means no depots are injected into the package at all, so every
 * manifest stops working at once and the library simply looks untouched. That
 * is why this keeps asking rather than giving up after one try.
 *
 * Termination is by progress, not by a count: a call that does not enlarge the
 * allocation will not enlarge it next time either, so this stops instead of
 * spinning. The attempt cap is a second belt, against a Grow that reports
 * success while doing nothing at all.
 *
 * Templated on the two operations so the growth policy under test is the same
 * code that ships. tools/ensure_room_test.cpp supplies fakes for both.
 */
template <typename CapacityFn, typename GrowFn>
bool EnsureRoom(uint32_t need, CapacityFn capacity, GrowFn grow)
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        const uint32_t have = capacity();
        if (have >= need) return true;
        if (!grow(static_cast<int>(need - have))) return false;
        // No progress: asking again would be an infinite loop.
        if (capacity() <= have) return false;
    }
    return capacity() >= need;
}

} // namespace HookUtil
