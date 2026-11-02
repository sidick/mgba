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
 * broad to use globally).
 *
 * IMPORTANT: do not write this as expf(re) * (cosf(im) + I * sinf(im)).
 * That calls cosf and sinf with the same argument, which is the exact
 * pattern this optimizer rewrites into a call to cexpf -- including
 * inside cexpf's own body, producing unconditional infinite recursion
 * (confirmed via -S: the compiled body contained "jsr _cexpf" calling
 * itself) and a stack overflow at runtime. __attribute__((no_builtin))
 * is not recognized on this GCC backend. Computing sin(im) as
 * cos(im - pi/2) avoids the pattern (no literal sinf() call) without
 * disabling any optimization. */
float complex cexpf(float complex z) {
	float re = crealf(z);
	float im = cimagf(z);
	float e = expf(re);
	float c = cosf(im);
	float s = cosf(im - (float) (M_PI / 2.0));
	return e * c + e * s * I;
}

#include <sys/types.h>

/* fsync/ftruncate are called unconditionally (no HAVE_* guard) from
 * src/util/vfs/vfs-fd.c and are absent from newlib on this toolchain.
 *
 * fsync: a safe no-op -- AmigaOS's dos.library Write() isn't buffered in
 * a way that needs an additional explicit flush beyond what newlib's own
 * I/O layer already does. */
int fsync(int fd) {
	(void) fd;
	return 0;
}

#include <dos/dos.h>
#include <proto/dos.h>

/* newlib's own fd table (src/sys/amigaos/open.c): __fh[fd] is the BPTR
 * dos.library file handle backing a given POSIX fd. Not declared in any
 * public header, same as _WBenchMsg etc. */
extern BPTR* __fh;
extern int __maxfh;

/* A no-op ftruncate() is NOT semantically safe here (unlike fsync): a
 * savestate/config file rewritten shorter than its previous contents
 * would retain old trailing bytes, corrupting whatever reads it back.
 * (Originally suspected, then ruled out, as the cause of a Pokemon
 * Emerald crash under volamos -- that turned out to be the cexpf
 * infinite-recursion bug above. This real implementation is still
 * correct and necessary on its own merits.) */
int ftruncate(int fd, long length) {
	if (fd < 0 || fd >= __maxfh || !__fh[fd]) {
		return -1;
	}
	if (SetFileSize(__fh[fd], length, OFFSET_BEGINNING) < 0) {
		return -1;
	}
	return 0;
}
