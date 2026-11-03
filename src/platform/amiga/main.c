/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "debug-log.h"
#include "gamepad.h"
#include "gui-platform.h"
#include "video-rtg.h"

#include "feature/gui/gui-runner.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/feature/commandline.h>

#include <stdlib.h>

#include <dos/dostags.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>

/* ROM is now optional on the command line (see _runBrowser()): without
 * one, mgba's shared ROM-browser/settings GUI opens instead. Quit is
 * reachable either way -- the native Project menu's "Quit mGBA" (both
 * modes) or the ROM browser's own menu, as well as the window's close
 * gadget in windowed mode. */
#define AMIGA_OPTIONS "FS:"
#define AMIGA_USAGE \
	"Amiga frontend options:\n" \
	"  ROM              Optional. Without one, opens mgba's ROM browser/\n" \
	"                   settings GUI instead of loading a specific ROM.\n" \
	"  -F               Open fullscreen (a dedicated RTG screen) instead of a window\n" \
	"  -S SCALE         Integer pixel-doubling scale. Fullscreen (-F) default:\n" \
	"                   auto -- picks the largest scale that fits the best\n" \
	"                   available RTG mode. Windowed default: 1 (never auto-\n" \
	"                   maximizes into the desktop). An explicit SCALE opens\n" \
	"                   the window/screen at exactly that multiple of GBA\n" \
	"                   resolution either way, with no further auto-fit.\n" \
	"\n" \
	"Keyboard mapping (loading a ROM directly):\n" \
	"  Cursor keys      D-pad\n" \
	"  X / Z            A / B\n" \
	"  A / S            L / R\n" \
	"  Return / Backspace  Start / Select\n" \
	"\n" \
	"Keyboard mapping (ROM browser/settings GUI, and its in-game pause menu):\n" \
	"  Cursor keys      Navigate\n" \
	"  Return           Select\n" \
	"  Backspace        Back (up a directory)\n" \
	"  Esc              Cancel / pause\n" \
	"\n" \
	"A joystick/CD32 pad/USB gamepad (via Poseidon) on the port printed\n" \
	"\"2\" is read too -- see src/platform/amiga/gamepad.c for the mapping."

/* Upper bound purely as a sanity check against a typo/garbage argument --
 * not a hardware limit. A window/screen this large would exceed most RTG
 * cards' own mode tables (and memory) long before this matters. */
#define AMIGA_MAX_SCALE 16

struct AmigaOpts {
	bool fullscreen;
	int scale;
};

static bool _parseAmigaOpts(struct mSubParser* parser, int option, const char* arg) {
	struct AmigaOpts* opts = parser->opts;
	switch (option) {
	case 'F':
		UNUSED(arg);
		opts->fullscreen = true;
		return true;
	case 'S': {
		int scale = atoi(arg);
		if (scale < 1 || scale > AMIGA_MAX_SCALE) {
			return false;
		}
		opts->scale = scale;
		return true;
	}
	default:
		return false;
	}
}

/* mgba's default (no logger registered) fallback in src/core/log.c does an
 * unbuffered printf()/vprintf() per mLog() call, with no level filtering --
 * fine on a desktop, but a ROM that trips a WARN-level message in a tight
 * loop (e.g. a byte-at-a-time OAM clear, each byte logging its own
 * "Cannot Store8 to OAM") can emit hundreds of lines in the time it'd take
 * to render a handful of frames. AmigaDebugLoggerInit() (debug-log.h)
 * filters down to FATAL+ERROR same as mStandardLogger would, but routes
 * through Copperline's uaelib debug-log trap instead of stdout -- so this
 * frontend's own diagnostics never need serial.device, leaving it free
 * for something else that wants exclusive use of it (e.g. a wire client
 * such as AmiPilotServer SERIAL). */
static struct mLogger* s_logger;

/* core->currentVideoSize() (mgba/core/core.h) needs the ROM already
 * loaded to answer correctly for a Super Game Boy ROM specifically
 * (whether its border is shown depends on both the ROM's own SGB flag
 * and the "gb.sgbBorders" config, neither known until mCoreLoadFile())
 * -- so this is called after mCoreLoadFile(), not right after
 * core->init(). Resizes the window for whatever just got loaded and
 * points the core at the (unmoved) pixel buffer once sized correctly,
 * reusing the exact VideoRTGReopen() mechanism the Settings menu's own
 * scale picker already relies on. `scale` should be the user's own
 * originally-requested value (0 for auto, or an explicit -S), not
 * whatever VideoRTGScale() last resolved to -- re-resolving "auto"
 * against the real (possibly smaller) content size can land on a larger
 * integer scale than GBA's own assumed size would have. */
static void _sizeForCore(struct mCore* core, bool fullscreen, int scale) {
	unsigned width, height;
	core->currentVideoSize(core, &width, &height);
	VideoRTGSetContentSize((int) width, (int) height);
	VideoRTGReopen(fullscreen, scale);
	core->setVideoBuffer(core, VideoRTGBuffer(), AMIGA_VIDEO_STRIDE);
}

/* The original (and still default, when a ROM path is given on the
 * command line) frontend: load exactly that ROM and run it, with the
 * native Project/Settings pulldown menu (menu.h) for Quit/Open ROM.../
 * About/scale -- no on-screen GUI at all. Unchanged by the addition of
 * _runBrowser() below. */
static int _runDirect(struct mArguments* args, bool fullscreen, int scale) {
	struct mCore* core = mCoreFind(args->fname);
	if (!core) {
		AmigaDebugLogf("Could not find a core to run %s\n", args->fname);
		return 1;
	}

	core->init(core);
	mCoreLoadFile(core, args->fname);
	_sizeForCore(core, fullscreen, scale);
	mCoreConfigInit(&core->config, "amiga");
	mCoreConfigLoad(&core->config);

	struct mCoreOptions opts = {};
	mCoreConfigMap(&core->config, &opts);
	mArgumentsApply(args, NULL, 0, &core->config);
	mCoreConfigLoadDefaults(&core->config, &opts);
	/* Matches src/platform/test/perf-main.c's own default: "detect" finds
	 * idle busy-wait loops in guest code at runtime, unlike the GBA core's
	 * own built-in default ("remove"), which mostly needs a matching
	 * per-game entry in mgba's override database to do anything -- our
	 * homebrew/test ROMs have none. Doesn't touch BIOS-region execution
	 * (GBASetActiveRegion in src/gba/memory.c explicitly excludes it), so
	 * it won't help a ROM that waits via a real VBlankIntrWait() BIOS
	 * call, only one that busy-polls memory directly instead. */
	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");
	mCoreLoadConfig(core);

	core->reset(core);

	bool quit = false;
	while (!quit) {
		quit = VideoRTGShouldQuit();
		if (quit) {
			break;
		}

		enum MenuAction action = VideoRTGMenuAction();
		switch (action) {
		case MENU_ACTION_QUIT:
			quit = true;
			break;
		case MENU_ACTION_OPEN_ROM: {
			char* path = MenuOpenROM(VideoRTGWindow());
			if (path) {
				/* Same mCore object reused for the new ROM -- correct for
				 * same-platform ROM switches (the common case), but not a
				 * real platform re-detection: a GBA core handed a GB ROM
				 * (or vice versa) here would misbehave. Re-detecting via
				 * mCoreFind() and recreating the core entirely would fix
				 * that properly; not done here -- a bigger, separate
				 * change, and a pre-existing limitation of this menu item,
				 * not something this resolution work introduces. Keeps the
				 * window at its current scale (VideoRTGScale()) rather
				 * than re-running auto-detection, so swapping ROMs doesn't
				 * surprise the user with a resized window unless the new
				 * ROM's own resolution actually differs (SGB border vs
				 * not, say). */
				core->unloadROM(core);
				mCoreLoadFile(core, path);
				_sizeForCore(core, fullscreen, VideoRTGScale());
				core->reset(core);
				free(path);
			}
			break;
		}
		case MENU_ACTION_ABOUT:
			MenuShowAbout(VideoRTGWindow());
			break;
		case MENU_ACTION_SCALE_1X:
		case MENU_ACTION_SCALE_2X:
		case MENU_ACTION_SCALE_3X:
		case MENU_ACTION_SCALE_AUTO: {
			/* 1x/2x/3x/Auto, in that enum order -- see menu.h. Auto maps
			 * to scale 0, same meaning VideoRTGInit()'s own `scale`
			 * parameter already gives it. */
			static const int kScaleForAction[] = { 1, 2, 3, 0 };
			int newScale = kScaleForAction[action - MENU_ACTION_SCALE_1X];
			VideoRTGReopen(fullscreen, newScale);
			break;
		}
		default:
			break;
		}
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
	return 0;
}

/* No ROM path given on the command line: mgba's shared ROM-browser/
 * settings GUI (src/feature/gui/gui-runner.c, the same one 3DS/PSP2/
 * Switch/Wii use) instead of failing outright. "amiga" as the config
 * port name matches _runDirect()'s own mCoreConfigInit() call above --
 * both paths share one settings/key-binding namespace. */
static int _runBrowser(void) {
	struct mGUIRunner runner = {0};
	mGUIInit(&runner, "amiga");
	AmigaGuiPlatformInit(&runner);
	mGUIRunloop(&runner);
	mGUIDeinit(&runner);
	return 0;
}

static int _realMain(int argc, char** argv) {
	s_logger = AmigaDebugLoggerInit();
	mLogSetDefaultLogger(s_logger);

	struct AmigaOpts amigaOpts = { false, 0 };
	struct mSubParser subparser = {
		.usage = AMIGA_USAGE,
		.parse = _parseAmigaOpts,
		.extraOptions = AMIGA_OPTIONS,
		.opts = &amigaOpts
	};

	/* No args.fname check here, unlike before: a ROM path is now
	 * optional -- without one, _runBrowser() below opens the ROM
	 * browser instead of _runDirect() loading it straight away. */
	struct mArguments args = {};
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
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

	if (!VideoRTGInit(amigaOpts.fullscreen, amigaOpts.scale)) {
		AmigaDebugLogf("Could not open an RTG screen/window (is Picasso96API.library installed?)\n");
		VideoRTGDeinit();
		mArgumentsDeinit(&args);
		return 1;
	}

	/* Not fatal if this fails (missing/ancient lowlevel.library): keyboard
	 * input alone is still a fully working frontend. */
	GamepadInit();

	int result = args.fname ? _runDirect(&args, amigaOpts.fullscreen, amigaOpts.scale) : _runBrowser();

	GamepadDeinit();
	VideoRTGDeinit();
	mArgumentsDeinit(&args);
	mLogSetDefaultLogger(NULL);
	AmigaDebugLoggerDeinit(s_logger);
	return result;
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
		AmigaDebugLogf("Warning: only %lu bytes of stack and no signal free to relaunch with more; running as-is.\n", stackSize);
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
		AmigaDebugLogf("Warning: only %lu bytes of stack and could not relaunch with more; running as-is.\n", stackSize);
		return _realMain(argc, argv);
	}

	Wait(1UL << s_relaunchDoneSignal);
	FreeSignal(s_relaunchDoneSignal);
	return s_relaunchExitCode;
}
