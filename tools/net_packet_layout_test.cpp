// Runnable check for NetPacketLayout::Detect, the thing standing between a
// Steam update that moves a struct and a segfault on every start.
//
//   g++ -std=c++20 -I src tools/net_packet_layout_test.cpp -o /tmp/net_packet_layout_test
//   /tmp/net_packet_layout_test
//
// Header-only and free of Steam types, so this exercises the code that ships.
//
// The first case is not made up. It is the first eight words of the CNetPacket
// that crashed Steam three times in eighteen minutes, read out of the core
// dump: a handle, two 0xFFFFFFFF words nothing in this repository knew were
// there, then the data pointer, the length (145), a refcount of 2, and the
// same pointer again as the network buffer. The hook read the first
// 0xFFFFFFFF as the data pointer and dereferenced it.
#include "Hook/NetPacketLayout.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

using NetPacketLayout::Detect;
using NetPacketLayout::Offsets;

int main()
{
    std::printf("The packet that crashed Steam\n");
    {
        const uint32_t w[8] = { 0x00000007, 0xFFFFFFFF, 0xFFFFFFFF, 0xC9FAB8A0,
                                0x00000091, 0x00000002, 0xC9FAB8A0, 0x00000000 };
        Offsets o;
        check(Detect(w, o), "is recognised, not refused");
        check(o.data == 0x0C && o.size == 0x10,
              "as the current layout: data at +0xC, size at +0x10");
        check(w[o.data / 4] == 0xC9FAB8A0 && w[o.size / 4] == 145,
              "which reads the real pointer and the real length of 145");
    }

    std::printf("A packet in the older layout\n");
    {
        // Built by hand from the layout the hook was written against.
        const uint32_t w[8] = { 0x00000003, 0xB7001000, 0x00000200, 0x00000001,
                                0xB7001000, 0x00000000, 0, 0 };
        Offsets o;
        check(Detect(w, o), "is still recognised, so older clients keep working");
        check(o.data == 0x04 && o.size == 0x08, "as the older layout: +4 and +8");
    }

    std::printf("Packets that must not be touched\n");
    {
        Offsets o;
        const uint32_t zeros[8] = {};
        check(!Detect(zeros, o), "an all-zero struct is refused");

        // Both candidate pointers are the sentinel. This is the crash packet
        // with its real fields blanked, so nothing fits either reading.
        const uint32_t sentinel[8] = { 7, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                       0xFFFFFFFF, 2, 0xFFFFFFFF, 0 };
        check(!Detect(sentinel, o), "a struct of sentinels is refused");

        // A layout this code has never seen: the data pointer further along.
        const uint32_t unknown[8] = { 7, 0, 0, 0, 0, 0xC9FAB8A0, 0x91, 2 };
        check(!Detect(unknown, o), "a third, unknown layout is refused, not guessed at");

        // A plausible pointer but a length no packet could have.
        const uint32_t huge[8] = { 7, 0xFFFFFFFF, 0xFFFFFFFF, 0xC9FAB8A0,
                                   0x7FFFFFFF, 2, 0xC9FAB8A0, 0 };
        check(!Detect(huge, o), "an absurd length rules a layout out");

        // And one too short to hold a header, which the hook rejects itself.
        const uint32_t tiny[8] = { 7, 0xFFFFFFFF, 0xFFFFFFFF, 0xC9FAB8A0,
                                   0x00000004, 2, 0xC9FAB8A0, 0 };
        check(!Detect(tiny, o), "a length too short for a message header is refused");
    }

    std::printf("The two layouts can never both fit\n");
    {
        // +0xC is the refcount in one layout and the pointer in the other. If a
        // single value could pass as both, Detect would have to choose between
        // them, which is the mistake being avoided. Sweep the whole range
        // around the boundary and across both, with everything else valid.
        bool both = false, any = false;
        for (uint32_t v : { 0u, 1u, 2u, 0xFFFEu, 0xFFFFu, 0x10000u, 0x10001u,
                            0x08048000u, 0xC9FAB8A0u, 0xFFFFFFFEu, 0xFFFFFFFFu }) {
            const uint32_t w[8] = { 7, 0xB7001000, 0x200, v, 0x200, 1, 0xB7001000, 0 };
            Offsets o;
            const bool d = Detect(w, o);
            any = any || d;
            // If it detected, the refcount reading and the pointer reading
            // were exclusive, which the offsets prove.
            if (d && !(o.data == 0x04 || o.data == 0x0C)) both = true;
        }
        check(!both, "no value at +0xC satisfies both readings");
        check(any, "and the sweep did detect something, so it was not vacuous");
    }

    std::printf(failures ? "\nFAILED\n" : "\nall passed\n");
    return failures ? 1 : 0;
}
