#pragma once

namespace SFPlatform::Detour {

    bool BeginTransaction();
    bool CommitTransaction();
    bool Attach(void** target, void* detour);
    bool Detach(void** target, void* detour);

} // namespace SFPlatform::Detour
