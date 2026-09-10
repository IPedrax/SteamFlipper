#pragma once

#include "dllmain.h"

#include <string>

namespace Hooks_Package {
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // One line on what the last injection attempt did, for the status page.
    // Everything this reports is already logged, and every one of those logs is
    // compiled out of Release, so without this a package that refused to hold
    // the depots is indistinguishable from an install that simply does nothing.
    std::string LicenseStatus();

}
