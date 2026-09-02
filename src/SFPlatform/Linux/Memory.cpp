#include "include/Memory.h"

#include "include/Log.h"

#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

namespace SFPlatform::Memory {
namespace {

struct PhdrCallbackData {
    void* targetHandle = nullptr;
    uint8_t* base = nullptr;
    size_t size = 0;
    bool found = false;
};

int DlIterateCallback(struct dl_phdr_info* info, size_t /*size*/, void* data) {
    auto* cbData = static_cast<PhdrCallbackData*>(data);

    // If handle matches or targetHandle is null (main module)
    struct link_map* lm = nullptr;
    if (cbData->targetHandle && dlinfo(cbData->targetHandle, RTLD_DI_LINKMAP, &lm) == 0 && lm) {
        if (lm->l_addr != info->dlpi_addr && (info->dlpi_name && strcmp(lm->l_name, info->dlpi_name) != 0)) {
            return 0; // Not this module
        }
    }

    cbData->base = reinterpret_cast<uint8_t*>(info->dlpi_addr);
    uintptr_t maxAddr = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            uintptr_t segEnd = info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz;
            if (segEnd > maxAddr) {
                maxAddr = segEnd;
            }
        }
    }
    cbData->size = maxAddr;
    cbData->found = true;
    return 1; // Stop iteration
}

} // namespace

std::optional<ModuleImage> GetModuleImage(DynamicLibrary::ModuleHandle module) {
    PhdrCallbackData data;
    data.targetHandle = module;
    dl_iterate_phdr(DlIterateCallback, &data);

    if (!data.found || !data.base || data.size == 0) {
        SFP_LOG_DEBUG("GetModuleImage(module={}) failed", module);
        return std::nullopt;
    }

    return ModuleImage{
        data.base,
        data.size,
    };
}

bool WriteExecutableByte(void* target, uint8_t value) {
    if (!target) {
        SFP_LOG_WARN("WriteExecutableByte: target is null");
        return false;
    }

    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return false;

    uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    uintptr_t pageStart = addr & ~(pageSize - 1);

    if (mprotect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SFP_LOG_WARN("WriteExecutableByte(target={}) mprotect(RWX) failed", target);
        return false;
    }

    *static_cast<uint8_t*>(target) = value;

    if (mprotect(reinterpret_cast<void*>(pageStart), pageSize, PROT_READ | PROT_EXEC) != 0) {
        SFP_LOG_WARN("WriteExecutableByte(target={}) mprotect(RX) restore failed", target);
    }

    __builtin___clear_cache(static_cast<char*>(target), static_cast<char*>(target) + 1);
    return true;
}

} // namespace SFPlatform::Memory
