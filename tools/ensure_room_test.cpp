// Runnable check for HookUtil::EnsureRoom, the loop that grows Steam's package
// app list until the depots fit.
//
//   g++ -std=c++20 -I src tools/ensure_room_test.cpp -o /tmp/ensure_room_test
//   /tmp/ensure_room_test
//
// Header-only and templated on its two operations, so this exercises the code
// that ships rather than a copy of it.
//
// Worth having because both failure modes are silent and total. Stop short and
// no depots are injected, so every manifest dies at once and the library just
// looks untouched. Fail to stop and the client hangs on startup inside a hook,
// which is worse. The middle case is the one that bit: Valve's Grow enlarges by
// its own policy, so a single call from a small base comes back short.
#include "Hook/EnsureRoom.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

// Steam's real policy as measured in this client: roughly a doubling, which
// ignores how much was actually asked for.
struct Doubling {
    uint32_t cap;
    int calls = 0;
    bool grow(int) { calls++; cap = cap ? cap * 2 : 8; return true; }
};

int main()
{
    std::printf("Already big enough\n");
    {
        Doubling m{2048};
        const bool ok = HookUtil::EnsureRoom(1181, [&] { return m.cap; },
                                             [&](int by) { return m.grow(by); });
        check(ok, "succeeds");
        check(m.calls == 0, "without growing anything, because there was room");
    }

    std::printf("One call would fall short\n");
    {
        // The real case: 981 depots wanted, a small starting allocation, and a
        // policy that doubles. One call lands nowhere near, and the old code
        // gave up here and injected nothing at all.
        Doubling m{196};
        const bool ok = HookUtil::EnsureRoom(1181, [&] { return m.cap; },
                                             [&](int by) { return m.grow(by); });
        check(ok, "keeps asking until it fits");
        check(m.cap >= 1181, "and the allocation really does hold the depots");
        check(m.calls > 1, "which took more than the single call it used to make");
    }

    std::printf("Growing from nothing\n");
    {
        Doubling m{0};
        const bool ok = HookUtil::EnsureRoom(981, [&] { return m.cap; },
                                             [&](int by) { return m.grow(by); });
        check(ok, "a zero-capacity list still gets there");
    }

    std::printf("A Grow that refuses\n");
    {
        int calls = 0;
        uint32_t cap = 10;
        const bool ok = HookUtil::EnsureRoom(
            1000, [&] { return cap; },
            [&](int) { calls++; return false; });
        check(!ok, "reports failure rather than pretending");
        check(calls == 1, "and asks once, not thirty-two times");
    }

    std::printf("A Grow that lies\n");
    {
        // Reports success, changes nothing. Without the progress check this is
        // an infinite loop inside a hook on Steam's startup path.
        int calls = 0;
        uint32_t cap = 10;
        const bool ok = HookUtil::EnsureRoom(
            1000, [&] { return cap; },
            [&](int) { calls++; return true; });
        check(!ok, "is caught rather than believed");
        check(calls == 1, "and stops immediately, so the client cannot hang");
    }

    std::printf("Growth too slow to finish\n");
    {
        // Progresses by one each time, so it never reaches the target inside
        // the attempt cap. Must give up, not spin.
        uint32_t cap = 0;
        int calls = 0;
        const bool ok = HookUtil::EnsureRoom(
            10000, [&] { return cap; },
            [&](int) { calls++; cap++; return true; });
        check(!ok, "gives up rather than looping forever");
        check(calls <= 32, "within the attempt cap");
    }

    std::printf(failures ? "\nFAILED\n" : "\nall passed\n");
    return failures ? 1 : 0;
}
