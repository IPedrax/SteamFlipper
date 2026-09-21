#pragma once

#include <string>
#include "dllmain.h"

namespace Hooks_Decryption {

#if defined(__linux__)
    // Where Steam's ConfigStore::GetBinary resolves to, or why it has not.
    // Read-only: reporting this never calls through the address.
    std::string GetBinaryStatus();

    // Attach the key-serving hook once Steam has built its config store.
    // Safe to call repeatedly; the first success latches.
    void TryInstallLate();
#endif
    // LoadDepotDecryptionKey hook: serves user-provided decryption keys for
    // depots configured via Lua.
    void Install();
    void Uninstall();

    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId);
}
