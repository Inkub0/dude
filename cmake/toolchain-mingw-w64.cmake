# Cross-compile DUDE for 64-bit Windows with mingw-w64 (host: Linux).
#
# Deps (Arch/CachyOS):
#   sudo pacman -S --needed mingw-w64-gcc          # compiler + windres
#   paru -S mingw-w64-sdl2 mingw-w64-openal        # AUR, prebuilt into the sysroot
#
# Use via build-win.sh, or manually:
#   cmake -S neo -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake
#
# For a 32-bit build, override with -DMINGW_PREFIX=i686-w64-mingw32.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT MINGW_PREFIX)
	set(MINGW_PREFIX x86_64-w64-mingw32)
endif()

set(CMAKE_C_COMPILER   ${MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}-windres)

# the mingw sysroot: where the AUR SDL2/OpenAL packages install their
# headers, import libs and DLLs (/usr/<prefix>/{include,lib,bin})
set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_PREFIX})

# never run target (Windows) programs on the host. Leave LIBRARY/INCLUDE/PACKAGE
# at the default (BOTH): the sysroot in CMAKE_FIND_ROOT_PATH is searched first,
# and the *DIR env hints below then resolve. Forcing these to ONLY double-reroots
# the hint paths and breaks the bundled sys/cmake/FindSDL2.cmake header search
# (it finds libSDL2 but not SDL.h).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# env hints the bundled/CMake find-modules honour: FindSDL2.cmake -> $ENV{SDL2DIR}
# (PATH_SUFFIXES include/SDL2), FindOpenAL -> $ENV{OPENALDIR}.
set(ENV{SDL2DIR}   /usr/${MINGW_PREFIX})
set(ENV{SDLDIR}    /usr/${MINGW_PREFIX})
set(ENV{OPENALDIR} /usr/${MINGW_PREFIX})
