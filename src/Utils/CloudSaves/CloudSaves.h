#pragma once

#include <cstdint>

// Native Steam Cloud save backend. Answers the Cloud.* RPCs the NetPacket hook
// intercepts for addappid()-unlocked games, out of a folder on this machine
// instead of Valve's servers.
//
// This exists because CloudRedirect cannot serve that role on Linux: its
// released cloud_redirect.so exports only CR_GetVersion and the crash-context
// pair, since the host API in cr_api.h is behind __declspec(dllexport). The
// interception in src/Hook is fine; only the backend behind it was missing.
//
// Storage layout, one subtree per account and app:
//   <steam>/steamflipper/cloudsaves/<accountId>/<appId>/<relative path>
//   <steam>/steamflipper/cloudsaves/<accountId>/<appId>.manifest
//
// File bytes do not travel inside the RPCs. The client uploads and downloads
// them over HTTP against whatever host the reply names, so the backend runs a
// loopback-only HTTP endpoint and points the client at it. That is the only
// path the protocol offers for moving save data.
//
// The signatures mirror CloudRedirectHost so the call site in src/Hook can be
// switched over with a minimal diff. Every entry point is a safe no-op unless
// [cloud].enabled is set and Initialize succeeded.
namespace CloudSaves {

    // Create the store, start the loopback endpoint, seed the app set from the
    // current addappid() list. Called once from the init worker thread with the
    // Steam root directory. Returns false when disabled or unusable.
    bool Initialize(const char* steamInstallPath);

    // True once the store and the loopback endpoint are up.
    bool IsActive();

    // Whether saves for this appid are served locally.
    bool IsApp(uint32_t appId);

    // Replace the served app set.
    void SetApps(const uint32_t* appIds, uint32_t count);

    // Re-push the current unlocked-app set. Called after a Lua hot-reload so
    // the served set tracks addappid() changes.
    void SyncAppSet();

    // Bridge from the NetPacket hook: answers one Cloud.* RPC. Returns false
    // when the method is not one we serve, so the caller chains to the
    // original. On true, respBuf/respLen hold the reply body and eresult holds
    // the EResult for the synthesized ServiceMethodResponse header.
    bool HandleRpc(const char* method, uint32_t appId, uint32_t accountId,
                   const uint8_t* reqBody, uint32_t reqLen,
                   uint8_t* respBuf, uint32_t respMaxLen,
                   uint32_t* respLen, int32_t* eresult);

    // Teardown. Called from DLL_PROCESS_DETACH.
    void Shutdown();

}
