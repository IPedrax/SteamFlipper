#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Where a CNetPacket keeps its data pointer and its size, and how to find out
 * without crashing the client when the answer has changed.
 *
 * The struct is Valve's and it moved. Between two Steam builds an 8-byte field
 * appeared after the connection handle, so everything behind it shifted:
 *
 *            older build        current build
 *   +0x00    m_hConnection      m_hConnection
 *   +0x04    m_pubData          (8 bytes, both 0xFFFFFFFF in a live packet)
 *   +0x08    m_cubData          ...
 *   +0x0C    m_cRef             m_pubData
 *   +0x10    m_pubNetworkBuffer m_cubData
 *   +0x14    m_pNext            m_cRef
 *   +0x18                       m_pubNetworkBuffer
 *   +0x1C                       m_pNext
 *
 * The hook read +4 as a pointer, got 0xFFFFFFFF, and dereferenced it. That was
 * a segfault inside Steam on the first packet of every session, on every start,
 * which is the worst shape a bug can take: total, immediate and deterministic,
 * with the cause in a struct nothing in this repository defines.
 *
 * So the offsets are not constants any more. They are read off a live packet
 * and checked against what a real one must look like, and a packet that fits
 * neither layout is left strictly alone. A build whose layout is unknown costs
 * the features that rewrite packets, and nothing else.
 *
 * Header-only and free of Steam types so tools/net_packet_layout_test.cpp can
 * feed it the bytes captured from the crash itself.
 */
namespace NetPacketLayout {

struct Offsets {
    size_t data = 0;   // byte offset of the uint8* data pointer
    size_t size = 0;   // byte offset of the uint32 length
};

inline constexpr Offsets kOlder   { 0x04, 0x08 };
inline constexpr Offsets kCurrent { 0x0C, 0x10 };

// How many 32-bit words of the struct Detect() needs to see.
inline constexpr size_t kWords = 8;

namespace detail {

// A pointer a real user-space buffer could have. Below 64 KiB is the zero page
// and its neighbours, and an all-ones value is a sentinel that Valve's own code
// writes into fields it has not filled in, as the crash dump showed.
inline bool PlausiblePointer(uint32_t p) { return p >= 0x10000u && p != 0xFFFFFFFFu; }

// Large enough for the biggest message Steam sends, small enough that no stray
// pointer passes. The hook itself refuses anything of 7 bytes or fewer, since
// it could not hold a header.
inline bool PlausibleSize(uint32_t n) { return n >= 8u && n <= (16u << 20); }

// A reference count of a packet in flight: at least the holder's own.
//
// The ceiling is one below PlausiblePointer's floor on purpose. Offset 0x0C is
// the refcount in one layout and the data pointer in the other, so a word that
// could pass as both would make the two layouts indistinguishable. With the
// ranges disjoint, no packet can fit both, and Detect() never has to choose.
inline bool PlausibleRefs(uint32_t n) { return n >= 1u && n <= 0xFFFFu; }

inline bool Fits(const uint32_t* w, const Offsets& o, size_t refsWord) {
    return PlausiblePointer(w[o.data / 4]) &&
           PlausibleSize(w[o.size / 4]) &&
           PlausibleRefs(w[refsWord]);
}

} // namespace detail

/**
 * Decide which layout `w` (the first kWords dwords of a live packet) has.
 *
 * Returns false when neither fits, or when both do. Guessing between two
 * plausible readings is how this bug was made, so ambiguity is a refusal.
 */
inline bool Detect(const uint32_t* w, Offsets& out) {
    const bool older   = detail::Fits(w, kOlder,   0x0C / 4);
    const bool current = detail::Fits(w, kCurrent, 0x14 / 4);
    if (older == current) return false;
    out = older ? kOlder : kCurrent;
    return true;
}

} // namespace NetPacketLayout
