#pragma once

#include <string>

// Data backends for the LUAFlipper panels that report machine state rather than
// Lua manifest state: the fixes the user dropped in, cloud save redirection, and
// the effective steamflipper.toml.
//
// Split out of LuaFlipperUI.cpp so that file stays about transport (CDP
// injection and the loopback API) and nothing else.
namespace LuaFlipperPages {

    // Returns a JSON document for the given API path, or an empty string if
    // the path is not one of ours. steamPath is the Steam root directory.
    std::string Render(const std::string& path, const std::string& steamPath);

} // namespace LuaFlipperPages
