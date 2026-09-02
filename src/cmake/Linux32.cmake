# Toolchain for the 32-bit (i386) build of SteamFlipper.
#
# The Linux Steam client is still a 32-bit binary (ubuntu12_32/steam) and maps
# the 32-bit steamclient.so / steamui.so, so hooking the client core requires a
# 32-bit module. The 64-bit build stays for game-side and Proton hooking; Steam
# itself ships gameoverlayrenderer.so in both arches for exactly this reason.
#
# Deliberately does NOT set CMAKE_SYSTEM_NAME: i386 binaries run natively on an
# x86_64 multilib host, and declaring a cross-compile would make CMake treat the
# generated protoc as non-executable and demand a separate host build of it.

set(CMAKE_C_FLAGS_INIT   "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
# funchook declares `LANGUAGES C ASM` and hand-writes prehook-i686-gas.S. ASM
# flags do not inherit from the C flags, so without this the assembler stays in
# 64-bit mode and rejects the 32-bit source ("operand type mismatch for push").
set(CMAKE_ASM_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-m32")

# Steer find_package(CURL/OpenSSL) at the multilib tree so it resolves
# /usr/lib32 instead of silently matching the host's 64-bit libraries.
set(CMAKE_FIND_LIBRARY_CUSTOM_LIB_SUFFIX "32")
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib32/pkgconfig")
