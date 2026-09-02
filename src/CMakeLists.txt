cmake_minimum_required(VERSION 3.20)
project(SteamFlipper VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(STEAMFLIPPER_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string embedded into SteamFlipper")

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
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/steam_messages.pb.cc"
           "${PROTO_GEN_DIR}/steam_messages.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
    COMMAND $<TARGET_FILE:protoc>
        "--cpp_out=$<$<CONFIG:Release>:lite:>${PROTO_GEN_DIR}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}" protoc
    COMMENT "Generating protobuf C++ sources"
    VERBATIM
)

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
    "${PROTO_GEN_DIR}/steam_messages.pb.cc"
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
