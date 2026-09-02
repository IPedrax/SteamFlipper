#include "include/Thread.h"

#include "include/Log.h"

#include <pthread.h>

#include <memory>

namespace SFPlatform::Thread {

NativeThreadHandle CurrentNativeThreadHandle() {
    return reinterpret_cast<NativeThreadHandle>(pthread_self());
}

bool StartDetached(std::function<uint32_t()> entry) {
    if (!entry) {
        SFP_LOG_WARN("StartDetached: entry is empty");
        return false;
    }

    auto* heapEntry = new std::function<uint32_t()>(std::move(entry));

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int result = pthread_create(
        &thread,
        &attr,
        [](void* param) -> void* {
            std::unique_ptr<std::function<uint32_t()>> fn(static_cast<std::function<uint32_t()>*>(param));
            uint32_t ret = (*fn)();
            return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
        },
        heapEntry);

    pthread_attr_destroy(&attr);

    if (result != 0) {
        SFP_LOG_WARN("pthread_create failed (errno={})", result);
        delete heapEntry;
        return false;
    }

    return true;
}

} // namespace SFPlatform::Thread
