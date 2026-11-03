/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gamepad.h"
#include "video-rtg.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/feature/commandline.h>

#include <stdio.h>

#include <dos/dostags.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* The only way to quit right now is the window's close gadget (windowed
 * mode) or Ctrl-C from a shell (fullscreen has no close gadget of its own
 * yet). */
#define AMIGA_OPTIONS "F"
#define AMIGA_USAGE \
	"Amiga frontend options:\n" \
	"  -F               Open fullscreen (a dedicated RTG screen) instead of a window\n" \
	"\n" \
	"Keyboard mapping:\n" \
	"  Cursor keys      D-pad\n" \
	"  X / Z            A / B\n" \
	"  A / S            L / R\n" \
	"  Return / Backspace  Start / Select\n" \
	"\n" \
	"A joystick/CD32 pad/USB gamepad (via Poseidon) on the port printed\n" \
	"\"2\" is read too -- see src/platform/amiga/gamepad.c for the mapping."

struct AmigaOpts {
	bool fullscreen;
};

static bool _parseAmigaOpts(struct mSubParser* parser, int option, const char* arg) {
	struct AmigaOpts* opts = parser->opts;
	UNUSED(arg);
	switch (option) {
	case 'F':
		opts->fullscreen = true;
		return true;
	default:
		return false;
	}
}

/* mgba's default (no logger registered) fallback in src/core/log.c does an
 * unbuffered printf()/vprintf() per mLog() call, with no level filtering --
 * fine on a desktop, but a ROM that trips a WARN-level message in a tight
 * loop (e.g. a byte-at-a-time OAM clear, each byte logging its own
 * "Cannot Store8 to OAM") can emit hundreds of lines in the time it'd take
 * to render a handful of frames. Worse over this frontend's console/SER:
 * redirect, which is far slower than a desktop terminal. A standard logger
 * filtered down to FATAL+ERROR keeps real problems visible without that
 * cost. */
static struct mStandardLogger s_logger;

static int _realMain(int argc, char** argv) {
	mStandardLoggerInit(&s_logger);
	s_logger.logToStdout = true;
	s_logger.d.filter->defaultLevels = mLOG_FATAL | mLOG_ERROR;
	mLogSetDefaultLogger(&s_logger.d);

	struct AmigaOpts amigaOpts = { false };
	struct mSubParser subparser = {
		.usage = AMIGA_USAGE,
		.parse = _parseAmigaOpts,
		.extraOptions = AMIGA_OPTIONS,
		.opts = &amigaOpts
	};

	struct mArguments args = {};
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
	if (!args.fname) {
		parsed = false;
	}
	if (!parsed || args.showHelp) {
		usage(argv[0], NULL, NULL, &subparser, 1);
		mArgumentsDeinit(&args);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		mArgumentsDeinit(&args);
		return 0;
	}

	if (!VideoRTGInit(amigaOpts.fullscreen)) {
		fprintf(stderr, "Could not open an RTG screen/window (is Picasso96API.library installed?)\n");
		VideoRTGDeinit();
		mArgumentsDeinit(&args);
		return 1;
	}

	/* Not fatal if this fails (missing/ancient lowlevel.library): keyboard
	 * input alone is still a fully working frontend. */
	GamepadInit();

	struct mCore* core = mCoreFind(args.fname);
	if (!core) {
		fprintf(stderr, "Could not find a core to run %s\n", args.fname);
		GamepadDeinit();
		VideoRTGDeinit();
		mArgumentsDeinit(&args);
		return 1;
	}

	core->init(core);
	core->setVideoBuffer(core, VideoRTGBuffer(), AMIGA_VIDEO_STRIDE);
	mCoreLoadFile(core, args.fname);
	mCoreConfigInit(&core->config, "amiga");
	mCoreConfigLoad(&core->config);

	struct mCoreOptions opts = {};
	mCoreConfigMap(&core->config, &opts);
	mArgumentsApply(&args, NULL, 0, &core->config);
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);

	core->reset(core);

	bool quit = false;
	while (!quit) {
		quit = VideoRTGShouldQuit();
		if (quit) {
			break;
		}
		core->setKeys(core, VideoRTGKeys() | GamepadKeys());
		core->runFrame(core);
		VideoRTGBlit();
	}

	mCoreConfigFreeOpts(&opts);
	mCoreConfigDeinit(&core->config);
	core->unloadROM(core);
	core->deinit(core);

	GamepadDeinit();
	VideoRTGDeinit();
	mArgumentsDeinit(&args);
	mLogSetDefaultLogger(NULL);
	mStandardLoggerDeinit(&s_logger);
	return 0;
}

/* AmigaDOS gives a CLI-launched process a 4000-byte default stack
 * (dos.library/CreateNewProc's own documented default) unless the caller
 * happens to have run a bigger "Stack N" first. mgba's ARM interpreter +
 * GBA core have deep enough call chains that 4000 bytes isn't enough --
 * confirmed the hard way: closing the window (see video-rtg.c) worked
 * fine with "Stack 500000" first, and hung hard (not a clean crash --
 * there's no stack guard page on AmigaOS, so an overflow silently
 * corrupts whatever's mapped just below the stack, which only causes a
 * wild jump once the corrupted value actually gets used, which in this
 * case wasn't until the close/cleanup path ran) with the 4000-byte
 * default.
 *
 * Rather than require every caller to remember "Stack 500000" first,
 * check our actual stack size at startup and relaunch ourselves as a new
 * process with a guaranteed-big one if it's too small -- the standard
 * AmigaOS idiom for this (newlib's crt0 has no libnix-style "set a
 * __stack global" mechanism to lean on instead; that's a libnix-only
 * convenience, confirmed not applicable here). NP_Entry runs the new
 * process within this *same* loaded code (no re-exec from disk), so
 * _relaunchMain can reach the original argc/argv via plain statics. */
#define AMIGA_MIN_STACK (512 * 1024)

static int s_relaunchArgc;
static char** s_relaunchArgv;
static int s_relaunchExitCode;
static struct Task* s_relaunchParent;
static UBYTE s_relaunchDoneSignal;

static void _relaunchMain(void) {
	s_relaunchExitCode = _realMain(s_relaunchArgc, s_relaunchArgv);
	Signal(s_relaunchParent, 1UL << s_relaunchDoneSignal);
}

int main(int argc, char** argv) {
	struct Task* self = FindTask(NULL);
	ULONG stackSize = (ULONG) self->tc_SPUpper - (ULONG) self->tc_SPLower;
	if (stackSize >= AMIGA_MIN_STACK) {
		return _realMain(argc, argv);
	}

	s_relaunchArgc = argc;
	s_relaunchArgv = argv;
	s_relaunchParent = self;
	s_relaunchDoneSignal = AllocSignal(-1);
	if (s_relaunchDoneSignal == (UBYTE) -1) {
		fprintf(stderr, "Warning: only %lu bytes of stack and no signal free to relaunch with more; running as-is.\n", stackSize);
		return _realMain(argc, argv);
	}

	struct Process* child = CreateNewProcTags(
		NP_Entry, (ULONG) _relaunchMain,
		NP_StackSize, (ULONG) AMIGA_MIN_STACK,
		NP_Name, (ULONG) "mgba-amiga",
		NP_Input, (ULONG) Input(),
		NP_Output, (ULONG) Output(),
		NP_CloseInput, FALSE,
		NP_CloseOutput, FALSE,
		TAG_DONE);
	if (!child) {
		FreeSignal(s_relaunchDoneSignal);
		fprintf(stderr, "Warning: only %lu bytes of stack and could not relaunch with more; running as-is.\n", stackSize);
		return _realMain(argc, argv);
	}

	Wait(1UL << s_relaunchDoneSignal);
	FreeSignal(s_relaunchDoneSignal);
	return s_relaunchExitCode;
}
