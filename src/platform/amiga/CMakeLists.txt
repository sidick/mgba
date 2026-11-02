# Bebbo's m68k-amigaos-gcc toolchain (clib2) provides POSIX fd semantics
# (open/read/write/lseek/close) on top of dos.library, so we can reuse the
# same vfs-fd.c backend the Wii/3DS/posix ports use rather than writing a
# native dos.library VFile backend.
#
# Directory listing (vfs-dirent.c) is NOT included: clib2's dirent.h is a
# deliberate stub (#error "<dirent.h> not supported") on this toolchain.
# Needed later for ROM browsing; will require a native dos.library
# Examine()/ExNext()-based implementation, not the POSIX opendir() shim.
list(APPEND OS_DEFINES ENABLE_VFS_FD COLOR_16_BIT COLOR_5_6_5)
list(APPEND CORE_VFS_SRC ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-fd.c)

# libnix's crt0 (-mcrt=nix) requires the application to define
# __nocommandline; PLATFORM_LIBRARY is a hook the top-level CMakeLists.txt
# already links into every executable target (headless, docgen, etc.) but
# that no other platform currently populates.
add_library(mgba-amiga-startup STATIC startup.c)
set(PLATFORM_LIBRARY mgba-amiga-startup PARENT_SCOPE)

set(OS_DEFINES ${OS_DEFINES} PARENT_SCOPE)
set(CORE_VFS_SRC ${CORE_VFS_SRC} PARENT_SCOPE)
set(VFS_SRC ${VFS_SRC} PARENT_SCOPE)
set(OS_LIB ${OS_LIB} PARENT_SCOPE)
