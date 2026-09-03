#pragma once

#include <functional>
#include <string>
#include <vector>

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

    // How to get a lua.tools bearer token, or "" when nobody is signed in.
    //
    // Injected rather than called directly because the session lives in the UI
    // module, which already depends on this one; this keeps the arrow pointing
    // one way. Without it only the keyless sources and Hubcap are offered,
    // which is exactly the state before anyone signs in.
    void SetTokenProvider(std::function<std::string()> provider);

    // Every source this build knows, in the order it will try them: the user's
    // configured preference first, then whatever was left out. The Sources page
    // draws from this rather than from the config, so a source added by an
    // update appears without anyone editing a list.
    std::vector<std::string> EffectiveOrder();

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
