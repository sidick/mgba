# Bebbo's m68k-amigaos-gcc toolchain (clib2) provides POSIX fd and dirent
# semantics (open/read/write/lseek/close, opendir/readdir/closedir) on top
# of dos.library, so we can reuse the same vfs-fd.c/vfs-dirent.c backends
# the Wii/3DS/posix ports use rather than writing a native dos.library
# VFile backend. (The older newlib-derived toolchain's dirent.h was a
# deliberate #error stub; clib2's is a real implementation.)
# IOAPI_NO_64: newlib doesn't implement fopen64/ftello64/fseeko64 (minizip's
# FOPEN_FUNC falls back to plain fopen without this, same fix Wii applies).
list(APPEND OS_DEFINES ENABLE_VFS_FD COLOR_16_BIT COLOR_5_6_5 IOAPI_NO_64)
list(APPEND CORE_VFS_SRC ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-fd.c)
list(APPEND VFS_SRC ${PROJECT_SOURCE_DIR}/src/util/vfs/vfs-dirent.c)

# startup.c supplies platform compat shims (see its own comments). It's
# folded into OS_SRC (compiled directly into libmgba itself, like the
# Windows/POSIX platforms' OS_SRC) rather than linked as a separate
# PLATFORM_LIBRARY archive: PLATFORM_LIBRARY is linked *before* libmgba in
# the top-level CMakeLists.txt's target_link_libraries() calls, so a
# single-pass linker never goes back to pull symbols (e.g. cexpf) from it
# once something inside libmgba.a turns out to need them.
list(APPEND OS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/startup.c)

# Belt-and-suspenders on top of startup.c's own cexpf rewrite (which
# avoids the literal cosf(x)/sinf(x)-same-argument pattern GCC's
# optimizer matches and replaces with a self-recursive call to cexpf --
# see that file's comments, and the upstream GCC bug this was reported
# as). No per-function pragma/attribute reliably suppresses this
# ("optimize"/"no_builtin" either aren't accepted for -fno-builtin or
# have no effect on this transform, tested); scoping the blunt
# -fno-builtin to just this one small file via CMake, rather than
# project-wide, is the only mechanism that reliably works regardless of
# what pattern a future compiler version might additionally recognize.
# Requires CMake 3.18+ for the DIRECTORY-qualified form (the only way to
# make a child directory's source-file property visible to the
# ${BINARY_NAME} target, which is created in the top-level
# CMakeLists.txt, not here).
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/startup.c
	DIRECTORY ${PROJECT_SOURCE_DIR}
	PROPERTIES COMPILE_OPTIONS "-fno-builtin")

set(OS_DEFINES ${OS_DEFINES} PARENT_SCOPE)
set(CORE_VFS_SRC ${CORE_VFS_SRC} PARENT_SCOPE)
set(VFS_SRC ${VFS_SRC} PARENT_SCOPE)
set(OS_LIB ${OS_LIB} PARENT_SCOPE)
set(OS_SRC ${OS_SRC} PARENT_SCOPE)
