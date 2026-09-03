#pragma once

#include <string>

// Downloads and installs a Lua manifest pack for one appid.
//
// Only the two sources that need no account are used: Ryuu and Sushi, which are
// LuaTools' own hardcoded fallbacks for a missing api.json. Everything else in
// that catalog sits behind a lua.tools bearer token with a daily cap, which is
// the user's to supply and not something this module holds, so it is out of
// reach here by design.
//
// Both entry points are synchronous, return a JSON document and never throw. A
// pack is a few megabytes over links that are often slow, so callers should not
// run them on a thread that has to stay responsive.
//
// Non-Linux builds compile to stubs that report an error.
namespace LuaFlipperDownload {

    // Which sources carry this appid, per the check_apis probe.
    //   {"appid":"381210","sources":[{"name":"Ryuu","status":"available"}]}
    // status is whatever the probe reported ("available"), "unavailable" when it
    // did not list the source, or "unknown" when the probe host itself could not
    // be reached - Sushi lives on GitHub and may still work in that case.
    // Returns {"error":".."} only for a malformed appid.
    std::string ProbeSources(const std::string& appId);

    // Fetch the pack for appId from `source` ("Ryuu" or "Sushi"), extract it and
    // install: .lua into <steamPath>/config/stplug-in, .manifest into
    // <steamPath>/depotcache. Every other entry is ignored. steamPath is the
    // Steam root directory.
    //   {"ok":true,"installed":3,"files":["381210.lua",..],"rejected":[]}
    // or {"error":"..","rejected":[..]} when nothing could be installed.
    // `rejected` carries one line per entry skipped for a reason worth showing:
    // a name that escapes the install directory, encryption, an unsupported
    // compression method, or a failed checksum.
    std::string Install(const std::string& appId, const std::string& source,
                        const std::string& steamPath);

} // namespace LuaFlipperDownload
