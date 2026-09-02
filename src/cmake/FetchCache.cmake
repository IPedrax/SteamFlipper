# Caches FetchContent's downloaded *source trees* at the repo root so they
# survive a `rm -rf build/`. Done by setting FETCHCONTENT_SOURCE_DIR_<NAME>
# to point at the cached copies — that skips FetchContent's populate step
# entirely (no download, no subbuild), which is also what lets us share the
# cache across generators (Ninja for the main project, VS for SteamAPIProxy)
# without hitting the "generator mismatch in *-subbuild/" error.
#
# Layout:
#   <repo>/.deps/<name>-src/   <-- canonical source for each dependency
#
# First configure (cache empty): FETCHCONTENT_BASE_DIR is pinned so the
# initial populate lands at the shared location.
# Subsequent configures: FETCHCONTENT_SOURCE_DIR_<NAME> overrides take
# precedence — populate is bypassed, subbuild + build live in the per-tree
# default ${CMAKE_BINARY_DIR}/_deps and don't conflict across generators.
#
# Override the cache location at configure time:
#   cmake -S src -B build -DSF_DEPS_DIR=/path/to/shared/cache
#
# Idempotent.
if(_SF_FETCH_CACHE_INITIALISED)
    return()
endif()
set(_SF_FETCH_CACHE_INITIALISED TRUE)

if(NOT DEFINED SF_DEPS_DIR)
    # CMAKE_CURRENT_LIST_DIR is src/cmake; ../../.deps is the repo root.
    get_filename_component(SF_DEPS_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../.deps" ABSOLUTE)
endif()

# For each known dependency, if a source dir is already cached at the shared
# location, point FetchContent at it so populate becomes a no-op.
# Which deps this platform actually fetches. Detours is Windows-only and
# funchook (plus its distorm disassembler) is Linux-only, so listing all of
# them unconditionally would leave _SF_ALL_CACHED permanently FALSE on either
# platform — which pins FETCHCONTENT_BASE_DIR to the shared cache and makes
# every dep's *build* tree shared too. That matters for the dual-arch Linux
# build: the 32-bit and 64-bit trees would fight over one libfunchook.a.
set(_SF_CACHED_DEPS lua spdlog protobuf tomlplusplus)
if(WIN32)
    list(APPEND _SF_CACHED_DEPS detours)
else()
    list(APPEND _SF_CACHED_DEPS funchook distorm)
endif()

set(_SF_ALL_CACHED TRUE)
foreach(_dep IN ITEMS ${_SF_CACHED_DEPS})
    string(TOUPPER "${_dep}" _UPPER)
    set(_src "${SF_DEPS_DIR}/${_dep}-src")
    if(IS_DIRECTORY "${_src}")
        set(FETCHCONTENT_SOURCE_DIR_${_UPPER} "${_src}" CACHE PATH
            "Pre-populated ${_dep} source dir" FORCE)
    else()
        set(_SF_ALL_CACHED FALSE)
    endif()
endforeach()

if(_SF_ALL_CACHED)
    message(STATUS "FetchContent: reusing cached sources at ${SF_DEPS_DIR}")
else()
    # At least one dep still needs to be downloaded. Direct the populate
    # output to the shared cache so the next configure can reuse it.
    set(FETCHCONTENT_BASE_DIR "${SF_DEPS_DIR}" CACHE PATH
        "Shared FetchContent cache (sources + first-time builds)" FORCE)
    message(STATUS "FetchContent: populating missing sources into ${SF_DEPS_DIR}")
endif()

# Skip the periodic git-fetch / tarball revalidation on configures after
# the initial populate.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip FetchContent update step on subsequent configures" FORCE)
