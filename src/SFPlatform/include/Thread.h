#pragma once

#include <cstdint>
#include <functional>

namespace SFPlatform::Thread {

    using NativeThreadHandle = void*;

    NativeThreadHandle CurrentNativeThreadHandle();
    bool StartDetached(std::function<uint32_t()> entry);

} // namespace SFPlatform::Thread
