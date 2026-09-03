#pragma once

// Injects the LUAFlipper client UI (nav tab + dropdown + panels) into Steam's
// CEF frontend over the Chrome DevTools Protocol.
//
// This is deliberately standalone: it does not require Millennium or any other
// frontend framework. Steam exposes CDP on 127.0.0.1:8080 whenever the marker
// file <Steam>/.cef-enable-remote-debugging exists; the installer creates it.
//
// Injection targets the CEF page titled "Steam" (the main client window). The
// nav is NOT in SharedJSContext, which holds an effectively empty document.
//
// All entry points are safe no-ops when [ui].enabled is false, when the marker
// file is absent, or when the UI assets are missing.
namespace LuaFlipperUI {

    // Start the background injector thread. Called once from the init worker,
    // after hooks are installed. steamInstallPath is the Steam root directory.
    //
    // Returns immediately; CEF usually takes tens of seconds to come up, so the
    // thread polls rather than blocking startup.
    void Initialize(const char* steamInstallPath);

} // namespace LuaFlipperUI
