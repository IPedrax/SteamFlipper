#include "Hooks_Decryption.h"
#include "HookMacros.h"
#include "dllmain.h"
#include <string>

namespace {

    void* g_pConfigStoreLocal = nullptr;

#if defined(__linux__)
    /*
     * Where ConfigStore::GetBinary lives, and why it is not a pattern.
     *
     * It has no VProf scope and no symbol, so the generator cannot derive it,
     * and unlike the licence pair it cannot simply be pinned either: Steam
     * calls it through a vtable, so the address only exists once the singleton
     * has been constructed.
     *
     * Both of Steam's own call sites, the read and the write, agree on the
     * shape. Reading the one that formats "...Depots\%u\DecryptionKey":
     *
     *     lea  <global>(%ebx),%eax     ; the singleton CPackageInfoCache hangs off
     *     mov  (%eax),%eax             ; the object
     *     lea  <OFF>(%eax),%edx        ; this = object + OFF
     *     mov  <OFF>(%eax),%eax        ; its vptr
     *     mov  <SLOT*4>(%eax),%eax     ; the method
     *     push $0x80 / buf / name / $1 / this
     *     call *%eax
     *
     * OFF and SLOT are per build and come from the pattern file, where the
     * generator read them out of exactly that sequence. They were constants
     * here, 0xD60 and 6, and Steam's next update made OFF 0x1098: the lookup
     * found no vtable at the old place and the hook stayed off. That was the
     * safe way to be wrong, and it is still only luck that it was safe, so the
     * numbers no longer live in this file.
     *
     * Resolved and reported before anything calls through it. An address that
     * is merely plausible is exactly what went wrong with the licence pair,
     * and a wrong call here runs on Steam's own config store.
     */
    std::string g_getBinaryStatus = "not attempted yet";

    // Read-only. Returns the address Steam would call, or nullptr while the
    // singleton is still being constructed.
    void* ResolveGetBinary() {
        void** owner = reinterpret_cast<void**>(
            PatternLoader::FindPattern(client_hModule, "CPackageInfoCacheGlobal"));
        if (!owner) {
            g_getBinaryStatus = "CPackageInfoCacheGlobal not resolved";
            return nullptr;
        }
        // Asked for before the singleton, so a pattern file that does not carry
        // them is reported at once instead of looking like a client that is
        // merely slow to start. Zero is never valid for either: offset zero is
        // the object itself and vtable slot zero is a destructor.
        const uintptr_t storeOff =
            PatternLoader::FindField(client_hModule, "CPackageInfoCacheGlobal", "config_store_offset");
        const uintptr_t slot =
            PatternLoader::FindField(client_hModule, "CPackageInfoCacheGlobal", "get_binary_slot");
        if (!storeOff || !slot) {
            g_getBinaryStatus = "off: this build's pattern file has no config-store "
                                "offset (re-run tools/install_linux.sh)";
            return nullptr;
        }

        void* object = *owner;
        if (!object) {
            g_getBinaryStatus = "waiting for Steam to construct the singleton";
            return nullptr;
        }
        void* store = static_cast<char*>(object) + storeOff;
        void** vtbl = *reinterpret_cast<void***>(store);
        if (!vtbl) {
            g_getBinaryStatus = "config store has no vtable pointer";
            return nullptr;
        }
        void* fn = vtbl[slot];
        if (!fn) {
            g_getBinaryStatus = "vtable slot " + std::to_string(slot) + " is empty";
            return nullptr;
        }
        char buf[96];
        if (void* base = SFPlatform::DynamicLibrary::GetModuleBase(client_hModule)) {
            snprintf(buf, sizeof(buf), "resolved, rva 0x%lX (not hooked yet)",
                     static_cast<unsigned long>(reinterpret_cast<uintptr_t>(fn) -
                                                reinterpret_cast<uintptr_t>(base)));
        } else {
            snprintf(buf, sizeof(buf), "resolved (not hooked yet)");
        }
        g_getBinaryStatus = buf;
        return fn;
    }
#endif

    HOOK_FUNC(ConfigStoreGetBinary, int32, void* pObject, EConfigStore eConfigStore, const char* KeyName, char* Key, uint32 KeySize) {
        if (eConfigStore == k_EConfigStoreUserLocal && pObject && !g_pConfigStoreLocal) {
            g_pConfigStoreLocal = pObject;
            LOG_DECRYPTIONKEY_DEBUG("ConfigStoreGetBinary: captured local ConfigStore at {}", g_pConfigStoreLocal);

        }
        std::string name(KeyName);
        LOG_DECRYPTIONKEY_DEBUG("ConfigStore::GetBinary called for pObject={}, eConfigStore={}, KeyName='{}'", 
                                    pObject, static_cast<uint32>(eConfigStore), name);
        // Expected shape: ".../<DepotId>\DecryptionKey"
        if (size_t last = name.find("\\DecryptionKey"); last != std::string::npos) {
            if (size_t start = name.find_last_of("\\", last - 1); start != std::string::npos) {
                AppId_t depotId = std::stoul(name.substr(start + 1, last - start - 1));
                if (const auto& key = LuaConfig::GetDecryptionKey(depotId); !key.empty()) {
                    if (KeySize >= key.size()) {
                        LOG_DECRYPTIONKEY_INFO("Providing decryption key for depot {}: {}", depotId,
                                               spdlog::to_hex(key.data(), key.data() + key.size()));
                        memcpy(Key, key.data(), key.size());
                        return static_cast<int32>(key.size());
                    }
                    LOG_DECRYPTIONKEY_WARN("Decryption key for depot {} is too large ({} bytes) for buffer ({} bytes)",
                                            depotId, key.size(), KeySize);
                }
            }
        }
        return oConfigStoreGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
    }

    std::vector<uint8_t> ReadConfigStoreLocalBinary(const std::string& keyName) {
        if (!g_pConfigStoreLocal || !oConfigStoreGetBinary) {
            LOG_DECRYPTIONKEY_WARN("GetConfigStoreLocalBinary: ConfigStoreGetBinary not ready, cannot get binary value");
            return {};
        }

        std::vector<uint8_t> value(1024);
        int32 result = oConfigStoreGetBinary(g_pConfigStoreLocal, k_EConfigStoreUserLocal,
                                             keyName.c_str(),
                                             reinterpret_cast<char*>(value.data()),
                                             static_cast<uint32>(value.size()));
        if (result <= 0) {
            LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: failed to read KeyName='{}'", keyName);
            return {};
        }

        value.resize(result);
        LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: got value for KeyName='{}' ({} bytes)",
                                keyName, value.size());
        return value;
    }
}

namespace Hooks_Decryption {

#if defined(__linux__)
    /*
     * Resolved on demand rather than at Install(), because the singleton this
     * hangs off does not exist yet when hooks are installed. Asking for the
     * status is what retries it, which keeps the retry where someone is
     * actually looking rather than on a timer nobody reads.
     */
    std::string GetBinaryStatus() {
        TryInstallLate();
        return g_getBinaryStatus;
    }

    /*
     * Attach once the singleton exists.
     *
     * Not at Install() time, because Steam has not built the config store
     * then. Called from a path Steam walks early and often, so the window
     * between the store appearing and the hook covering it stays small.
     * Idempotent: the first success latches and later calls do nothing.
     */
    void TryInstallLate() {
        if (oConfigStoreGetBinary) return;
        void* fn = ResolveGetBinary();
        if (!fn) return;

        oConfigStoreGetBinary = reinterpret_cast<ConfigStoreGetBinary_t>(fn);
        if (!SFPlatform::Detour::Attach(
                reinterpret_cast<void**>(&oConfigStoreGetBinary),
                reinterpret_cast<void*>(hkConfigStoreGetBinary))) {
            oConfigStoreGetBinary = nullptr;
            g_getBinaryStatus = "resolved but could not be hooked";
            LOG_DECRYPTIONKEY_WARN("ConfigStoreGetBinary: attach failed");
            return;
        }
        g_getBinaryStatus = "serving keys from the manifests";
        LOG_DECRYPTIONKEY_INFO("ConfigStoreGetBinary: hooked at {}", fn);
    }
#endif

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(ConfigStoreGetBinary);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(ConfigStoreGetBinary);
        UNHOOK_END();
    }

    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId) {
        std::vector<uint8_t> ticket = ReadConfigStoreLocalBinary(std::format("apptickets\\{}", appId));
        if (ticket.empty()) {
            LOG_DECRYPTIONKEY_DEBUG("no cached ticket for AppId {}", appId);
            return ticket;
        }
        LOG_DECRYPTIONKEY_DEBUG("got cached ticket for AppId {} ({} bytes)", appId, ticket.size());
        return ticket;
    }
}
