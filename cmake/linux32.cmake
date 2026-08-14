# Toolchain file for building a 32 bit (i686) Linux executable with a multilib
# GCC/Clang, e.g.  cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/linux32.cmake ../neo
#
# Needs the 32 bit runtime *and* development files of the dependencies; on Arch /
# CachyOS that is (at least):
#   gcc-multilib lib32-glibc lib32-sdl2(-compat) lib32-openal lib32-curl
#   lib32-vulkan-icd-loader  +  the lib32 ICD of your GPU driver
#     (lib32-nvidia-utils / lib32-vulkan-radeon / lib32-mesa)
#
# shaderc has no lib32 package, so configure with -DDUDE_RUNTIME_ARB_COMPILER=OFF
# (custom-ARB / mod shader stages then degrade to skip on the Vulkan backend).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_FLAGS_INIT   "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-m32")

# look for libraries in the multilib directories first, but keep /usr/include
# (headers are arch independent on a multilib system) searchable
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/lib32 /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# CMake would otherwise probe the 64 bit lib dirs
set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
set(CMAKE_SIZEOF_VOID_P 4)

set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib32/pkgconfig:/usr/share/pkgconfig")
