cmake_minimum_required(VERSION 3.20)
# The version lives in the VERSION file at the repo root, not here, so bumping a
# release is editing one line of plain text. The updater fetches that same file
# from the branch and compares it, which means the number shown in the client and
# the number people compare against cannot drift apart.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" STEAMFLIPPER_VERSION_RAW)
string(STRIP "${STEAMFLIPPER_VERSION_RAW}" STEAMFLIPPER_VERSION_RAW)
project(SteamFlipper VERSION ${STEAMFLIPPER_VERSION_RAW} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
# FORCE, because this is derived from the VERSION file rather than a knob. A
# plain CACHE entry is written once and then never updated, so the first
# configure pinned the number forever: editing VERSION bumped the file, the tag
# and the updater's idea of the release while the client kept reporting the old
# one, with a rebuild that looked like it had worked.
set(STEAMFLIPPER_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version string embedded into SteamFlipper" FORCE)

# Allow CMAKE_MSVC_RUNTIME_LIBRARY to control runtime selection for all targets,
# including dependencies pulled in via FetchContent.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if(MSVC)
    # Static MSVC runtime everywhere, so the resulting DLL has no extra runtime
    # dependencies. Must be set BEFORE FetchContent_MakeAvailable so the fetched
    # deps (Lua, Detours, spdlog) inherit it.
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Every dependency is a static lib that ends up inside a shared library, so
# they all need PIC — not just SteamFlipper itself. Without this, ELF links
# fail on non-PIC TLS relocations (R_X86_64_TPOFF32) out of libprotobuf. Must
# be set BEFORE FetchContent_MakeAvailable so the fetched deps inherit it.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Build provenance: the commit the module is built from.
#
# The updater compares this sha against the branch head, so a build made
# outside a checkout must say so rather than report a sha it cannot back up.
# ---------------------------------------------------------------------------
set(SF_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(STEAMFLIPPER_GIT_SHA "unknown")
set(STEAMFLIPPER_GIT_BRANCH "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SF_REPO_ROOT}" rev-parse --short HEAD
        OUTPUT_VARIABLE SF_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE SF_GIT_RC)
    if(SF_GIT_RC EQUAL 0 AND SF_GIT_SHA)
        set(STEAMFLIPPER_GIT_SHA "${SF_GIT_SHA}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SF_REPO_ROOT}" rev-parse --abbrev-ref HEAD
        OUTPUT_VARIABLE SF_GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE SF_GIT_RC)
    if(SF_GIT_RC EQUAL 0 AND SF_GIT_BRANCH)
        set(STEAMFLIPPER_GIT_BRANCH "${SF_GIT_BRANCH}")
    endif()
endif()
message(STATUS "SteamFlipper source: ${STEAMFLIPPER_GIT_BRANCH}@${STEAMFLIPPER_GIT_SHA}")

# The sha is resolved at configure time, so without this a rebuild after a
# commit keeps reporting the sha the tree was first configured at -- and the
# updater would then call an up-to-date install "behind". HEAD covers a branch
# switch, the ref file covers a new commit on the branch already checked out.
foreach(SF_GIT_WATCH "${SF_REPO_ROOT}/.git/HEAD"
                     "${SF_REPO_ROOT}/.git/refs/heads/${STEAMFLIPPER_GIT_BRANCH}")
    if(EXISTS "${SF_GIT_WATCH}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SF_GIT_WATCH}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SteamFlipperBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/SteamFlipperBuildInfo.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Dependency recipes (FetchContent-backed, cached at <repo>/.deps).
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(STEAMFLIPPER_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
include(Lua)
include(Spdlog)
include(Protobuf)
include(Tomlplusplus)
include(LogMacros)

# Build the host code generators (ipc_codegen) in their own binary subtree so
# their executables never land in the shipped output directory. EXCLUDE_FROM_ALL
# means only the tools actually depended on (ipc_codegen) get built here.
add_subdirectory("${STEAMFLIPPER_TOOLS_DIR}" "${CMAKE_BINARY_DIR}/tools" EXCLUDE_FROM_ALL)
include("${STEAMFLIPPER_TOOLS_DIR}/cmake/IPCCodegen.cmake")

set(IPC_IDL "${CMAKE_CURRENT_SOURCE_DIR}/Steam/IPCMessages.steamd")
# Per-config output dir, mirroring the protobuf recipe below: a Multi-Config
# build invokes the generator once per config, and a single shared output file
# would ping-pong between the Release/Debug graphs and force needless rebuilds.
steamflipper_add_ipc_codegen(IPC_GEN
    IDL "${IPC_IDL}"
    CPP_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# Protobuf code generation — one variant per config, from the same .proto:
#
#   Debug   → full Message     (protoc --cpp_out)       → links libprotobuf
#   Release → lite MessageLite (protoc --cpp_out=lite:) → links libprotobuf-lite
# ---------------------------------------------------------------------------
# steam_messages carries the client/service messages the hooks touch,
# cloud_messages the Steam Cloud subset the native save backend parses. They
# import nothing from each other, so each is generated on its own.
set(PROTO_NAMES steam_messages cloud_messages)
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
set(PROTO_GEN_SRCS "")

foreach(proto IN LISTS PROTO_NAMES)
    add_custom_command(
        OUTPUT "${PROTO_GEN_DIR}/${proto}.pb.cc"
               "${PROTO_GEN_DIR}/${proto}.pb.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
        COMMAND $<TARGET_FILE:protoc>
            "--cpp_out=$<$<CONFIG:Release>:lite:>${PROTO_GEN_DIR}"
            "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
            "${CMAKE_CURRENT_SOURCE_DIR}/proto/${proto}.proto"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proto/${proto}.proto" protoc
        COMMENT "Generating protobuf C++ sources for ${proto}.proto"
        VERBATIM
    )
    list(APPEND PROTO_GEN_SRCS "${PROTO_GEN_DIR}/${proto}.pb.cc")
endforeach()

# ---------------------------------------------------------------------------
# SFPlatform — internal static library that owns the OS-specific backends.
# ---------------------------------------------------------------------------
add_subdirectory(SFPlatform)

# ---------------------------------------------------------------------------
# SteamFlipper — the hook module injected into Steam (DLL on Windows, SO on Linux).
# ---------------------------------------------------------------------------
add_library(SteamFlipper SHARED
    dllmain.cpp

    # Shared utilities
    Utils/Tickets/AppTicket.cpp
    Utils/Tickets/LegacyCDKey.cpp
    Utils/Tickets/EticketClient.cpp
    Utils/Config/Config.cpp
    Utils/Config/ConfigFileWatcher.cpp
    Utils/Config/LuaConfig.cpp
    Utils/Config/LuaFileWatcher.cpp
    Utils/CloudRedirect/CloudRedirectHost.cpp
    Utils/CloudSaves/CloudSaves.cpp
    Utils/LuaFlipperUI/LuaFlipperUI.cpp
    Utils/LuaFlipperUI/LuaFlipperPages.cpp
    Utils/LuaFlipperUI/LuaFlipperDownload.cpp
    Utils/HookSupport/VehCommon.cpp
    Utils/Logging/Log.cpp
    Utils/SteamMetadata/IPCLoader.cpp
    Utils/SteamMetadata/ManifestClient.cpp
    Utils/SteamMetadata/Mirror.cpp
    Utils/SteamMetadata/PatternLoader.cpp
    Utils/SteamMetadata/RemoteToml.cpp
    Utils/SteamMetadata/StatsClient.cpp
    Utils/SteamMetadata/SteamDiagnostics.cpp
    Utils/Tokeer/TokeerBridge.cpp
    Utils/Tokeer/TokeerUri.cpp
    Utils/Update/AppUpdater.cpp

    # Pipe subsystem: generic handshake registry + ProcessInspector core, plus
    # self-contained features that own their primitives.
    Pipe/PipeManager.cpp
    Pipe/ProcessInspector.cpp
    Pipe/Features/DenuvoAuth/DenuvoAuth.cpp
    Pipe/Features/DenuvoAuth/ProtectionScan.cpp
    Pipe/Features/Injection/Injection.cpp

    # Generated IPC message structs (depends on IPCMessages.steamd via add_custom_command)
    "${IPC_GEN}"

    # Per-category hook modules
    Hook/HookManager.cpp
    Hook/Hooks_CallBack.cpp
    Hook/Hooks_Decryption.cpp
    Hook/Hooks_IPC.cpp
    Hook/Hooks_IPC_ISteamUser.cpp
    Hook/Hooks_IPC_ISteamUtils.cpp
    Hook/Hooks_KeyValues.cpp
    Hook/Hooks_Manifest.cpp
    Hook/Hooks_Misc.cpp
    Hook/Hooks_NetPacket.cpp
    Hook/PendingAPICalls.cpp
    Hook/Hooks_SteamUI.cpp
    Hook/Hooks_Package.cpp

    # protobuf generated sources (full in Debug, lite in Release)
    ${PROTO_GEN_SRCS}
)

set_target_properties(SteamFlipper PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX ""
    OUTPUT_NAME "SteamFlipper"
)

if(NOT WIN32)
    # An LD_PRELOAD object is inserted at the FRONT of the global symbol lookup
    # scope, so anything it exports interposes on the host process and every
    # library the host loads. Statically linking protobuf, lua and spdlog into
    # this module otherwise re-exports their entire surface — ~31k symbols —
    # and Steam, which carries its own copies, binds to ours instead and dies
    # during startup. Nothing external calls into this module on Linux (the
    # work happens in the ELF constructor), so hide everything: -fvisibility
    # covers our own objects, --exclude-libs covers the static archives.
    set_target_properties(SteamFlipper PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    target_link_options(SteamFlipper PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# Header search path — per-config include directory
target_include_directories(SteamFlipper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
)

target_link_libraries(SteamFlipper PRIVATE
    SFPlatform
    lua_static
    $<$<CONFIG:Debug>:libprotobuf>
    $<$<CONFIG:Release>:libprotobuf-lite>
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
    $<$<PLATFORM_ID:Windows>:Shell32>
    # Manifest packs arrive as zip archives; LuaFlipperDownload inflates them.
    $<$<PLATFORM_ID:Linux>:z>
)

# Logging is compiled in only for Debug; Release reduces LOG_* to no-ops.
target_compile_definitions(SteamFlipper PRIVATE
    $<$<CONFIG:Debug>:STEAMFLIPPER_LOGGING_ENABLED>
)

set(SF_ETICKET_URL "" CACHE STRING
    "Backend URL for on-demand eticket minting (empty disables the feature)")
if(SF_ETICKET_URL)
    target_compile_definitions(SteamFlipper PRIVATE
        SF_ETICKET_URL="${SF_ETICKET_URL}")
endif()

set(SF_TOKEER_URL "" CACHE STRING
    "Base URL for the Tokeer code server (bst:// bridge)")
if(SF_TOKEER_URL)
    target_compile_definitions(SteamFlipper PRIVATE
        SF_TOKEER_URL="${SF_TOKEER_URL}")
endif()

# ---------------------------------------------------------------------------
# Windows loader-hijack DLLs (Windows-only)
# ---------------------------------------------------------------------------
if(WIN32)
    # dwmapi.dll hijack
    add_library(dwmapi SHARED
        dwmapi/dwmapi.cpp
    )

    # xinput1_4.dll hijack
    add_library(xinput1_4 SHARED
        xinput1_4/xinput1_4.cpp
        xinput1_4/xinput1_4.def
    )
endif()
