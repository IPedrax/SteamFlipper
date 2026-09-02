# ---------------------------------------------------------------------------
# SFPlatform — internal static library that owns the OS-specific backends.
# ---------------------------------------------------------------------------

set(OSTPLATFORM_COMMON_SOURCES
    Encoding.cpp
    Numbers.cpp
    Log.cpp
)

if(WIN32)
    include(Detours)

    set(OSTPLATFORM_PLATFORM_SOURCES
        Windows/Dialog.cpp
        Windows/Detour.cpp
        Windows/DirectoryWatch.cpp
        Windows/DynamicLibrary.cpp
        Windows/Hash.cpp
        Windows/Http.cpp
        Windows/Memory.cpp
        Windows/ByteSearch.cpp
        Windows/PE.cpp
        Windows/Process.cpp
        Windows/RemoteProcess.cpp
        Windows/SteamCredentialStore.cpp
        Windows/Thread.cpp
        Windows/Trap.cpp
    )

    set(OSTPLATFORM_PLATFORM_LIBS
        winhttp
        Bcrypt
        detours
    )
else()
    include(Funchook)

    set(OSTPLATFORM_PLATFORM_SOURCES
        Linux/Dialog.cpp
        Linux/Detour.cpp
        Linux/DirectoryWatch.cpp
        Linux/DynamicLibrary.cpp
        Linux/Hash.cpp
        Linux/Http.cpp
        Linux/Memory.cpp
        Linux/ByteSearch.cpp
        Linux/PE.cpp
        Linux/Process.cpp
        Linux/RemoteProcess.cpp
        Linux/SteamCredentialStore.cpp
        Linux/Thread.cpp
        Linux/Trap.cpp
    )

    find_package(CURL REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(Threads REQUIRED)

    # Deliberately NOT linking CURL::libcurl. Steam runs inside the
    # steam-runtime, which pins an older libcurl.so.4 (CURL_OPENSSL_3) ahead of
    # the host's (CURL_OPENSSL_4); a link-time dependency makes the loader
    # demand a version tag the runtime cannot supply and kill the Steam client
    # at startup. Linux/Http.cpp dlopens curl instead, so we take the headers
    # from the found package but none of its link interface.
    set(OSTPLATFORM_PLATFORM_LIBS
        OpenSSL::Crypto
        Threads::Threads
        ${CMAKE_DL_LIBS}
        funchook
    )
    set(OSTPLATFORM_CURL_INCLUDE_DIRS
        $<TARGET_PROPERTY:CURL::libcurl,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

add_library(SFPlatform STATIC
    ${OSTPLATFORM_COMMON_SOURCES}
    ${OSTPLATFORM_PLATFORM_SOURCES}
)

set_target_properties(SFPlatform PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(SFPlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..   # src/ — for cross-module private headers
        ${OSTPLATFORM_CURL_INCLUDE_DIRS} # headers only; curl is dlopen'd (see above)
)

target_link_libraries(SFPlatform PRIVATE
    ${OSTPLATFORM_PLATFORM_LIBS}
)

# Logging is compiled in only for Debug; Release reduces SFP_LOG_* to no-ops.
target_compile_definitions(SFPlatform PRIVATE
    $<$<CONFIG:Debug>:STEAMFLIPPER_LOGGING_ENABLED>
)
