# Defines a static `funchook` target by fetching upstream funchook sources.
# Idempotent — safe to include() from multiple CMakeLists.
if(TARGET funchook OR TARGET funchook-static)
    return()
endif()

include(FetchCache)
include(FetchContent)

FetchContent_Declare(
    funchook
    GIT_REPOSITORY https://github.com/kubo/funchook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

set(FUNCHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(FUNCHOOK_DISASM "distorm" CACHE STRING "" FORCE)

FetchContent_MakeAvailable(funchook)

if(TARGET funchook-static AND NOT TARGET funchook)
    add_library(funchook ALIAS funchook-static)
endif()
