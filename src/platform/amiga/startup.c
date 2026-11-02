/* Do NOT define __nocommandline here. Under -mcrt=nix (libnix), it's a
 * flag the application sets; but we build with -mcrt=newlib, where
 * __nocommandline is instead the name of newlib's own real init function
 * (src/sys/amigaos/__nocommandline.c, ADD2INIT(__nocommandline, -40))
 * that parses the raw AmigaDOS command line into argc/argv. Defining our
 * own symbol of that name silently replaces newlib's real one at link
 * time -- exactly the footgun newlib's own source comments on: "If you
 * replace this by an own reference called __nocommandline you get no
 * commandline arguments." (Learned the hard way: every program here was
 * seeing argc == 0 until this got removed.) */

#include <complex.h>
#include <math.h>

/* Neither newlib nor clib2 on this toolchain implement cexpf, but GCC's
 * optimizer recognizes the cosf(x)/sinf(x) pair in src/gba/bios.c's
 * affine-transform helpers and silently lowers it to a cexpf() call
 * regardless of -fno-builtin-cexpf (verified: that flag doesn't suppress
 * this specific transform, only the blunt -fno-builtin does, which is too
 * broad to use globally). */
float complex cexpf(float complex z) {
	return expf(crealf(z)) * (cosf(cimagf(z)) + I * sinf(cimagf(z)));
}

#include <sys/types.h>

/* fsync/ftruncate are called unconditionally (no HAVE_* guard) from
 * src/util/vfs/vfs-fd.c and are absent from newlib on this toolchain.
 *
 * fsync: a safe no-op -- AmigaOS's dos.library Write() isn't buffered in
 * a way that needs an additional explicit flush beyond what newlib's own
 * I/O layer already does.
 *
 * ftruncate: NOT semantically safe as a no-op (a real implementation
 * needs a dos.library SetFileSize() call against the fd's underlying
 * file handle) -- stubbed here only to reach a first working link; a
 * savestate/config file rewritten shorter than its previous contents
 * will currently retain the old trailing bytes. Needs real
 * implementation before relying on VFS writes that shrink a file. */
int fsync(int fd) {
	(void) fd;
	return 0;
}

int ftruncate(int fd, long length) {
	(void) fd;
	(void) length;
	return 0;
}
