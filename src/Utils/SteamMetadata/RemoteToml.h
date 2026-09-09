#pragma once
#include <string>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        std::string body;
        std::string sha256;

        // Why a failed fetch failed, which is the difference between advice
        // that helps and advice that wastes somebody's evening. A mirror that
        // answers 404 has no file for this build and none of them will; a
        // mirror that never answers says nothing about whether the file
        // exists, only that this machine could not ask.
        bool        reached   = false;  // some mirror completed a request
        long        status    = 0;      // status from the last mirror tried
    };

    // Fetch remote TOML first, then fall back to the exact local cache entry.
    Result Fetch(const Request& request);

} // namespace RemoteToml
