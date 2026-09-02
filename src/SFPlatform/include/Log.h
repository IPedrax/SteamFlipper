#pragma once

// Small logging facade for SFPlatform. The host installs a sink after its
// own logger is ready; Release builds compile SFP_LOG_* to no-ops.

#include <cstdint>
#include <string_view>

#ifdef STEAMFLIPPER_LOGGING_ENABLED
#include <format>
#endif

namespace SFPlatform::Log {

enum class Level : uint8_t { Trace, Debug, Info, Warn, Error };

struct Source {
    const char* file = nullptr;
    int line = 0;
    const char* function = nullptr;
};

// Host-injected receiver. `msg` is already formatted.
using Sink = void (*)(Level level, const Source& src, std::string_view msg);

void SetSink(Sink sink) noexcept;
void Dispatch(Level level, const Source& src, std::string_view msg) noexcept;

} // namespace SFPlatform::Log

#ifdef STEAMFLIPPER_LOGGING_ENABLED
#define SFP_LOG(lvl, ...)                                            \
    ::SFPlatform::Log::Dispatch(                                     \
        (lvl),                                                        \
        ::SFPlatform::Log::Source{__FILE__, __LINE__, __func__},     \
        ::std::format(__VA_ARGS__))
#else
#define SFP_LOG(lvl, ...) ((void)0)
#endif

#define SFP_LOG_TRACE(...) SFP_LOG(::SFPlatform::Log::Level::Trace, __VA_ARGS__)
#define SFP_LOG_DEBUG(...) SFP_LOG(::SFPlatform::Log::Level::Debug, __VA_ARGS__)
#define SFP_LOG_INFO(...)  SFP_LOG(::SFPlatform::Log::Level::Info,  __VA_ARGS__)
#define SFP_LOG_WARN(...)  SFP_LOG(::SFPlatform::Log::Level::Warn,  __VA_ARGS__)
#define SFP_LOG_ERROR(...) SFP_LOG(::SFPlatform::Log::Level::Error, __VA_ARGS__)
