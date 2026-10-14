#pragma once

#include <string>

namespace Hooks_NetPacket {
    void Install();
    void Uninstall();

    // Whether the packet struct was recognised, for the status page. The state
    // worth seeing is the unrecognised one: Steam moved the struct once, the
    // result was a crash on every start, and the next move should show up as a
    // line here instead.
    std::string PacketLayoutStatus();
}
