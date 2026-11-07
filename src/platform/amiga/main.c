/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "audio-ahi.h"
#include "debug-log.h"
#include "gamepad.h"
#include "gui-platform.h"
#include "video-rtg.h"

#include "feature/gui/gui-runner.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>

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
/* Fullscreen and scale are the shared mSubParserGraphicsInit() options
 * (-f/--fullscreen, -1..-8/--scale) every other mgba port already uses
 * -- see _realMain()'s graphics subparser below -- rather than this
 * port rolling its own flag letters for the same two concepts. Only
 * -A (AHI device unit) is genuinely Amiga-specific, so that's all this
 * port's own subparser still handles. */
#define AMIGA_OPTIONS "A:"
#define AMIGA_USAGE \
	"Amiga frontend options:\n" \
	"  ROM              Optional. Without one, opens mgba's ROM browser/\n" \
	"                   settings GUI instead of loading a specific ROM.\n" \
	"  -A UNIT          AHI device unit to open (0-3). Default: AHI's own\n" \
	"                   default unit. Each unit has its own independently-\n" \
	"                   configured audio mode/driver in SYS:Prefs/AHI, so this\n" \
	"                   lets you target a specific one (e.g. a unit known to\n" \
	"                   be set to Paula) instead of whatever the system\n" \
	"                   default happens to be.\n" \
	"\n" \
	"Without -f, a window is still upgraded to fullscreen automatically if\n" \
	"Workbench's own screen isn't a suitable HiColor/TrueColor RTG mode.\n" \
	"Without --scale, fullscreen defaults to auto (the largest scale that\n" \
	"fits the best available RTG mode); windowed defaults to 1x (never\n" \
	"auto-maximizes into the desktop).\n" \
	"\n" \
	"Keyboard mapping (loading a ROM directly):\n" \
	"  Cursor keys      D-pad\n" \
	"  X / Z            A / B\n" \
	"  A / S            L / R\n" \
	"  Return / Backspace  Start / Select\n" \
	"\n" \
	"Project menu (Right-Amiga shortcuts): Open ROM.../RA-O, Reset/RA-R,\n" \
	"Pause/RA-P, Mute/RA-M, Quick Save/RA-S, Quick Load/RA-L, Screenshot,\n" \
	"Quit/RA-Q. Settings menu: 1x/2x/3x/Auto scale, Toggle Fullscreen,\n" \
	"Save Settings (remembers scale, fullscreen, mute, and -A across runs --\n" \
	"an explicit -f/--scale/-A flag always overrides whatever was last saved).\n" \
	"Audio output (if any) goes wherever ahi.device is configured to --\n" \
	"see SYS:Prefs/AHI to pick a different driver/card.\n" \
	"\n" \
	"Keyboard mapping (ROM browser/settings GUI, and its in-game pause menu):\n" \
	"  Cursor keys      Navigate\n" \
	"  Return           Select\n" \
	"  Backspace        Back (up a directory)\n" \
	"  Esc              Cancel / pause\n" \
	"\n" \
	"A joystick/CD32 pad/USB gamepad (via Poseidon) on the port printed\n" \
	"\"2\" is read too -- see src/platform/amiga/gamepad.c for the mapping."

/* AHI's own device units (0-3, plus AHI_NO_UNIT -- see devices/ahi.h)
 * each carry an independently-configured audio mode/driver preference
 * via SYS:Prefs/AHI's own per-unit settings. CMD_WRITE itself has no
 * way to pick a specific audio mode/driver (confirmed from AHI's own
 * autodocs -- that's deliberately the system's job), but the *unit*
 * number IS something OpenDevice() takes directly, so exposing it here
 * is the one real lever this frontend has for "target a specific
 * configured output chain" without reimplementing AHI's own
 * Preferences editor. Only 0-3 are real, persistently-configurable
 * units; AHI_NO_UNIT (255) is for low-level-only access this frontend
 * doesn't use, so it's not accepted here. */
#define AMIGA_MAX_AHI_UNIT 3

struct AmigaOpts {
	/* -1 means "not given" -- AmigaAudioInit() treats that as
	 * AHI_DEFAULT_UNIT itself (devices/ahi.h isn't included here, to
	 * keep AHI specifics inside audio-ahi.c). */
	int ahiUnit;
	/* Set by _parseAmigaOpts() when -A was actually given on the command
	 * line -- _realMain() only fills this in from the "Save Settings"
	 * menu's saved value (see AMIGA_SETTING_* below) when it wasn't, so
	 * an explicit CLI flag always wins over whatever was last saved. */
	bool ahiUnitGiven;
};

static bool _parseAmigaOpts(struct mSubParser* parser, int option, const char* arg) {
	struct AmigaOpts* opts = parser->opts;
	switch (option) {
	case 'A': {
		int unit = atoi(arg);
		if (unit < 0 || unit > AMIGA_MAX_AHI_UNIT) {
			return false;
		}
		opts->ahiUnit = unit;
		opts->ahiUnitGiven = true;
		return true;
	}
	default:
		return false;
	}
}

/* Keys the "Save Settings" menu item (MENU_ACTION_SAVE_SETTINGS,
 * _runDirect() below) writes to, and _realMain() reads back at the next
 * startup -- all under this port's own "ports.amiga" config section
 * (mCoreConfigInit()'s `port` argument), so they share config.ini with
 * everything else this port already stores there without colliding with
 * mCoreOptions' own generic keys (which this frontend's fullscreen/scale
 * don't map onto anyway -- VideoRTGInit() needs a decision before any
 * core/mCoreOptions exists at all). fullscreen/scale here mirror the
 * shared -f/--scale flags' own meaning (see AMIGA_OPTIONS's own comment
 * above) rather than inventing Amiga-specific semantics for them. */
#define AMIGA_SETTING_FULLSCREEN "amigaFullscreen"
#define AMIGA_SETTING_SCALE "amigaScale"
#define AMIGA_SETTING_AHI_UNIT "amigaAhiUnit"
/* No CLI flag exists for mute (nor does any other mgba port expose one
 * -- it's menu/UI-only everywhere), so this one is always loaded from
 * whatever was last saved, with no "given on the command line" case to
 * override it. */
#define AMIGA_SETTING_MUTED "amigaMuted"

/* The settings "Save Settings" persists and _realMain() resolves before
 * VideoRTGInit()/AmigaAudioInit() need a final decision -- fullscreen
 * and scale share the shared graphics subparser's own meaning (0
 * scale/multiplier means "not given", same as that subparser's own
 * default), ahiUnit is this port's own -A, and muted has no CLI flag at
 * all (see AMIGA_SETTING_MUTED above). */
struct AmigaSettings {
	bool fullscreen;
	int scale;
	int ahiUnit;
	bool muted;
};

/* Loads whatever "Save Settings" last wrote (if anything) into any of
 * `settings`' fields whose matching flag was NOT given on the command
 * line this run (muted has no such flag, so it's always loaded) -- an
 * explicit CLI flag always overrides a saved value, never the other way
 * around. Uses its own, short-lived mCoreConfig rather than a loaded
 * ROM's: this runs in _realMain() before VideoRTGInit()/
 * AmigaAudioInit(), well before any core exists to hang one off of. */
static void _loadAmigaOpts(struct AmigaSettings* settings, bool fullscreenGiven, bool scaleGiven, bool ahiUnitGiven) {
	struct mCoreConfig config;
	mCoreConfigInit(&config, "amiga");
	mCoreConfigLoad(&config);

	if (!fullscreenGiven) {
		bool fullscreen;
		if (mCoreConfigGetBoolValue(&config, AMIGA_SETTING_FULLSCREEN, &fullscreen)) {
			settings->fullscreen = fullscreen;
		}
	}
	if (!scaleGiven) {
		int scale;
		if (mCoreConfigGetIntValue(&config, AMIGA_SETTING_SCALE, &scale) && scale >= 0) {
			settings->scale = scale;
		}
	}
	if (!ahiUnitGiven) {
		int ahiUnit;
		if (mCoreConfigGetIntValue(&config, AMIGA_SETTING_AHI_UNIT, &ahiUnit) && ahiUnit >= -1 && ahiUnit <= AMIGA_MAX_AHI_UNIT) {
			settings->ahiUnit = ahiUnit;
		}
	}
	bool muted;
	if (mCoreConfigGetBoolValue(&config, AMIGA_SETTING_MUTED, &muted)) {
		settings->muted = muted;
	}

	mCoreConfigDeinit(&config);
}

/* The Settings menu's "Save Settings" action: writes the four values
 * _loadAmigaOpts() reads back at the next startup. Takes `core` (rather
 * than its own standalone mCoreConfig) so this shares the same
 * already-loaded in-memory config a ROM-specific setting change might
 * also be pending in -- saving through it instead of a second, freshly-
 * loaded instance avoids clobbering any such change with stale data. */
static void _saveAmigaOpts(struct mCore* core, bool fullscreen, int scale, int ahiUnit, bool muted) {
	mCoreConfigSetIntValue(&core->config, AMIGA_SETTING_FULLSCREEN, fullscreen ? 1 : 0);
	mCoreConfigSetIntValue(&core->config, AMIGA_SETTING_SCALE, scale);
	mCoreConfigSetIntValue(&core->config, AMIGA_SETTING_AHI_UNIT, ahiUnit);
	mCoreConfigSetIntValue(&core->config, AMIGA_SETTING_MUTED, muted ? 1 : 0);
	mCoreConfigSave(&core->config);
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
/* core->reset()'s own GB implementation only associates the video
 * renderer with the core's video subsystem if a buffer was already set
 * (`if (gbcore->renderer.outputBuffer) { ... }`, src/gb/core.c) -- call
 * this BEFORE core->reset(), or the core runs with no renderer wired up
 * at all (it'll still execute, just never produce a frame). The pointer
 * itself never changes across a later VideoRTGReopen() (same fixed
 * VideoRTGBuffer() allocation regardless of content size), so there's
 * no need to call this again after a resize. */
static void _bindVideoBuffer(struct mCore* core) {
	core->setVideoBuffer(core, VideoRTGBuffer(), AMIGA_VIDEO_STRIDE);
}

/* core->currentVideoSize() only answers correctly once core->reset() has
 * actually run: GBModel detection from the ROM header (including a Super
 * Game Boy ROM's own border-eligibility) happens there, not at
 * mCoreLoadFile() -- beforehand, gb->model is still GB_MODEL_AUTODETECT
 * (0xFF, mgba/gb/interface.h), which has every model bit set including
 * GB_MODEL_SGB, so a query before reset() always takes
 * _GBCoreCurrentVideoSize()'s SGB-bordered branch regardless of the
 * ROM's real type -- confirmed live: a plain non-SGB ROM query before
 * reset() came back 256x224 (SGB's own size) instead of the correct
 * 160x144. So this runs AFTER core->reset(), resizing only the window
 * (VideoRTGReopen() never touches the already-bound buffer pointer, see
 * _bindVideoBuffer()'s own comment -- same mechanism the Settings menu's
 * live scale change already relies on). */
static void _sizeForCore(struct mCore* core, bool fullscreen, int scale) {
	unsigned width, height;
	core->currentVideoSize(core, &width, &height);
	VideoRTGSetContentSize((int) width, (int) height);
	VideoRTGReopen(fullscreen, scale);
}

/* Loads `path` into *corePtr, creating or replacing the core as needed --
 * unlike a plain core->unloadROM()+mCoreLoadFile() reload (fine for same-
 * platform ROM swaps, but silently wrong for a GBA core handed a GB ROM
 * or vice versa), this detects the new file's actual platform first
 * (mCoreIsCompatible() on a quick VFile peek, not the heavier mCoreFind()
 * -- this runs on every "Open ROM...", not just once at startup) and only
 * reuses *corePtr when it already matches. On a platform change (or the
 * very first call, *corePtr == NULL), the old core (if any) is fully torn
 * down -- config included, since struct mCoreOptions/mCore are matched
 * pairs -- and a fresh one built from scratch via mCoreCreate(), with the
 * same config/defaults setup _runDirect() used to do once at startup
 * inline. *opts is the matching struct mCoreOptions for *corePtr -- freed
 * and rebuilt alongside it on a platform change, left alone on a same-
 * platform reload. Returns false (nothing changed) if the file isn't a
 * ROM this build recognizes at all. */
static bool _loadRom(struct mCore** corePtr, struct mCoreOptions* opts, const char* path, struct mArguments* args, bool fullscreen, int scale) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	enum mPlatform platform = vf ? mCoreIsCompatible(vf) : mPLATFORM_NONE;
	if (vf) {
		vf->close(vf);
	}
	if (platform == mPLATFORM_NONE) {
		AmigaDebugLogf("Could not find a core to run %s\n", path);
		return false;
	}

	struct mCore* core = *corePtr;
	if (core && core->platform(core) == platform) {
		/* No explicit unloadROM() call here -- mCoreLoadFile() already
		 * does that itself, as its own first step. */
		if (!mCoreLoadFile(core, path)) {
			/* mCoreIsCompatible() above only sniffed the platform --
			 * mCoreLoadFile() itself can still fail (truncated/corrupt
			 * file, I/O error) and returns false when it does. Bail out
			 * here, before touching video/audio binding or resetting a
			 * core that has no ROM loaded -- *corePtr still points at
			 * the same, now ROM-less but otherwise valid core, same as
			 * if the caller had never asked to load anything. */
			AmigaDebugLogf("Could not load %s\n", path);
			return false;
		}
		_bindVideoBuffer(core);
		AmigaAudioBindCore(core);
		core->reset(core);
		_sizeForCore(core, fullscreen, scale);
		return true;
	}

	struct mCore* newCore = mCoreCreate(platform);
	if (!newCore) {
		AmigaDebugLogf("Could not create a core to run %s\n", path);
		return false;
	}
	if (core) {
		mCoreConfigFreeOpts(opts);
		mCoreConfigDeinit(&core->config);
		core->unloadROM(core);
		core->deinit(core);
	}

	core = newCore;
	*corePtr = core;
	core->init(core);
	/* Checked below, after config setup -- not bailed out of early,
	 * since *corePtr already points at this core and every later
	 * _loadRom() call (success or failure) assumes it has an
	 * initialized config to tear down/reuse. A failed load still
	 * leaves a fully-formed, reusable, just ROM-less core. */
	bool loaded = mCoreLoadFile(core, path);
	_bindVideoBuffer(core);
	AmigaAudioBindCore(core);
	mCoreConfigInit(&core->config, "amiga");
	mCoreConfigLoad(&core->config);

	*opts = (struct mCoreOptions) {};
	mCoreConfigMap(&core->config, opts);
	mArgumentsApply(args, NULL, 0, &core->config);
	mCoreConfigLoadDefaults(&core->config, opts);
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

	if (!loaded) {
		/* Same reasoning as the same-platform branch above: don't reset
		 * or resize a core that has no ROM loaded. */
		AmigaDebugLogf("Could not load %s\n", path);
		return false;
	}
	core->reset(core);
	_sizeForCore(core, fullscreen, scale);
	return true;
}

/* Quick save/load use one fixed slot (".ss0" next to the ROM itself)
 * rather than mCoreSaveState()/mCoreLoadState()'s own slot machinery --
 * those need core->dirs.state populated via mDirectorySetOpenPath(),
 * which needs ENABLE_DIRECTORIES (unset for this build, like every
 * other platform's own CMake config -- it's dead everywhere in this
 * tree, not an Amiga-specific gap). mCoreSaveStateNamed()/
 * mCoreLoadStateNamed() take a plain VFile instead and have no such
 * dependency, so this just builds the sibling path by hand -- the same
 * "%s.ss%i" convention mCoreGetState() itself uses, just computed from
 * the ROM path this frontend already has rather than core->dirs.baseName. */
#define AMIGA_QUICK_SAVE_SLOT 0

static void _quickSave(struct mCore* core, const char* romPath) {
	char path[PATH_MAX + 8];
	snprintf(path, sizeof(path), "%s.ss%d", romPath, AMIGA_QUICK_SAVE_SLOT);
	struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_RDWR);
	if (!vf) {
		AmigaDebugLogf("Could not open %s for quick save\n", path);
		return;
	}
	if (!mCoreSaveStateNamed(core, vf, SAVESTATE_ALL)) {
		AmigaDebugLogf("Quick save to %s failed\n", path);
	}
	vf->close(vf);
}

static void _quickLoad(struct mCore* core, const char* romPath) {
	char path[PATH_MAX + 8];
	snprintf(path, sizeof(path), "%s.ss%d", romPath, AMIGA_QUICK_SAVE_SLOT);
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		AmigaDebugLogf("No quick save to load at %s\n", path);
		return;
	}
	if (!mCoreLoadStateNamed(core, vf, SAVESTATE_ALL)) {
		AmigaDebugLogf("Quick load from %s failed\n", path);
	}
	vf->close(vf);
}

/* Same reasoning as the quick-save slot above: mCoreTakeScreenshot()
 * (the auto-numbering convenience wrapper) is also ENABLE_DIRECTORIES-
 * gated, so this calls mCoreTakeScreenshotVF() directly on a VFile
 * this frontend opens itself. One fixed name (overwriting any previous
 * screenshot) rather than mCoreTakeScreenshot()'s own numbered-suffix
 * scan -- simpler, and good enough for "grab the current frame". */
static void _quickScreenshot(struct mCore* core, const char* romPath) {
	char path[PATH_MAX + 8];
	snprintf(path, sizeof(path), "%s.png", romPath);
	struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_RDWR);
	if (!vf) {
		AmigaDebugLogf("Could not open %s for screenshot\n", path);
		return;
	}
	if (!mCoreTakeScreenshotVF(core, vf)) {
		AmigaDebugLogf("Screenshot to %s failed\n", path);
	}
	vf->close(vf);
}

/* The original (and still default, when a ROM path is given on the
 * command line) frontend: load exactly that ROM and run it, with the
 * native Project/Settings pulldown menu (menu.h) for Quit/Open ROM.../
 * About/scale -- no on-screen GUI at all. Unchanged by the addition of
 * _runBrowser() below. */
static int _runDirect(struct mArguments* args, bool fullscreen, int scale, int ahiUnit, bool muted) {
	struct mCore* core = NULL;
	struct mCoreOptions opts = {};
	if (!_loadRom(&core, &opts, args->fname, args, fullscreen, scale)) {
		return 1;
	}

	/* The quick-save/load/screenshot helpers above need the currently
	 * loaded ROM's own path to build a sibling filename from -- tracked
	 * here (not re-derived from the core) since mCore itself doesn't
	 * keep the path it was loaded from around. */
	char romPath[PATH_MAX];
	strncpy(romPath, args->fname, sizeof(romPath) - 1);
	romPath[sizeof(romPath) - 1] = '\0';

	/* `muted` is whatever "Save Settings" last wrote (default false) --
	 * apply it to the audio process and sync the menu's own checkmark to
	 * match, the same way MenuInit()'s own caller already does for the
	 * scale group (see video-rtg.c). */
	AmigaAudioSetMuted(muted);
	MenuSetChecked(VideoRTGWindow(), MENU_ACTION_MUTE, muted);

	bool paused = false;
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
				/* _loadRom() detects the new file's own platform and
				 * recreates the core if it differs from the current one
				 * (a GBA core can't just be handed a GB ROM, or vice
				 * versa) -- see its own comment. Reopens at
				 * VideoRTGRequestedScale() (the user's own last explicit
				 * choice -- 0 for Auto), not VideoRTGScale() (the
				 * resolved number): reusing the resolved number would
				 * silently turn "Auto" into a fixed scale the first time
				 * the user swaps ROMs, even though they never asked for
				 * a fixed number -- and "Auto" genuinely wants to
				 * re-resolve against the new ROM's own content size
				 * anyway (a platform change, or SGB border toggling, can
				 * change what the best fit even is). */
				if (_loadRom(&core, &opts, path, args, fullscreen, VideoRTGRequestedScale())) {
					strncpy(romPath, path, sizeof(romPath) - 1);
					romPath[sizeof(romPath) - 1] = '\0';
					/* A fresh ROM is never paused, regardless of whatever
					 * state the previous one was left in. */
					paused = false;
					MenuSetChecked(VideoRTGWindow(), MENU_ACTION_PAUSE, false);
				}
				free(path);
			}
			break;
		}
		case MENU_ACTION_ABOUT:
			MenuShowAbout(VideoRTGWindow());
			break;
		case MENU_ACTION_RESET:
			core->reset(core);
			break;
		case MENU_ACTION_PAUSE:
			paused = !paused;
			MenuSetChecked(VideoRTGWindow(), MENU_ACTION_PAUSE, paused);
			break;
		case MENU_ACTION_MUTE:
			muted = !muted;
			AmigaAudioSetMuted(muted);
			MenuSetChecked(VideoRTGWindow(), MENU_ACTION_MUTE, muted);
			break;
		case MENU_ACTION_QUICK_SAVE:
			_quickSave(core, romPath);
			break;
		case MENU_ACTION_QUICK_LOAD:
			_quickLoad(core, romPath);
			break;
		case MENU_ACTION_SCREENSHOT:
			_quickScreenshot(core, romPath);
			break;
		case MENU_ACTION_TOGGLE_FULLSCREEN: {
			bool wasFullscreen = fullscreen;
			fullscreen = !fullscreen;
			if (!VideoRTGReopen(fullscreen, VideoRTGRequestedScale())) {
				/* VideoRTGReopen() tears down whatever was open before
				 * attempting the new state, so a failure here (e.g.
				 * fullscreen Auto hitting the Z3660 synthesized-CUSTOM-
				 * mode limitation documented in VideoRTGReopen()'s own
				 * fullscreen branch) would otherwise leave no window at
				 * all. Revert to the state that was working a moment
				 * ago instead of leaving the app stuck windowless. */
				fullscreen = wasFullscreen;
				VideoRTGReopen(fullscreen, VideoRTGRequestedScale());
			}
			break;
		}
		case MENU_ACTION_SCALE_1X:
		case MENU_ACTION_SCALE_2X:
		case MENU_ACTION_SCALE_3X:
		case MENU_ACTION_SCALE_AUTO: {
			/* 1x/2x/3x/Auto, in that enum order -- see menu.h. Auto maps
			 * to scale 0, same meaning VideoRTGInit()'s own `scale`
			 * parameter already gives it. */
			static const int kScaleForAction[] = { 1, 2, 3, 0 };
			int newScale = kScaleForAction[action - MENU_ACTION_SCALE_1X];
			/* VideoRTGScale() is always resolved (never 0/Auto), so this
			 * is a real, known-working scale to fall back to -- same
			 * revert-on-failure reasoning as Toggle Fullscreen above. */
			int oldScale = VideoRTGScale();
			if (!VideoRTGReopen(fullscreen, newScale)) {
				VideoRTGReopen(fullscreen, oldScale);
			}
			break;
		}
		case MENU_ACTION_SAVE_SETTINGS:
			/* VideoRTGRequestedScale() (0 for Auto, matching
			 * AMIGA_SETTING_SCALE's own meaning), not VideoRTGScale() --
			 * same "save what the user actually asked for, not today's
			 * resolved number" reasoning MENU_ACTION_OPEN_ROM's own
			 * comment above gives for reusing it on a ROM reload. */
			_saveAmigaOpts(core, fullscreen, VideoRTGRequestedScale(), ahiUnit, muted);
			break;
		default:
			break;
		}
		if (quit) {
			break;
		}

		/* Paused: skip advancing the emulated machine, but keep blitting
		 * (and keep polling IDCMP/menu above) so the window stays
		 * responsive and the last real frame stays visible rather than
		 * freezing mid-draw. */
		if (!paused) {
			core->setKeys(core, VideoRTGKeys() | GamepadKeys());
			core->runFrame(core);
		}
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

	/* The shared -f/--fullscreen, -1..-8/--scale subparser every other
	 * mgba port already registers -- see AMIGA_OPTIONS's own comment
	 * above for why this port no longer rolls its own flag letters for
	 * the same two concepts. */
	struct mGraphicsOpts graphicsOpts;
	struct mSubParser graphicsSubparser;
	mSubParserGraphicsInit(&graphicsSubparser, &graphicsOpts);

	struct AmigaOpts amigaOpts = { -1, false };
	struct mSubParser amigaSubparser = {
		.usage = AMIGA_USAGE,
		.parse = _parseAmigaOpts,
		.extraOptions = AMIGA_OPTIONS,
		.opts = &amigaOpts
	};

	struct mSubParser subparsers[2] = { graphicsSubparser, amigaSubparser };

	/* No args.fname check here, unlike before: a ROM path is now
	 * optional -- without one, _runBrowser() below opens the ROM
	 * browser instead of _runDirect() loading it straight away. */
	struct mArguments args = {};
	bool parsed = mArgumentsParse(&args, argc, argv, subparsers, 2);
	if (!parsed || args.showHelp) {
		usage(argv[0], NULL, NULL, subparsers, 2);
		mArgumentsDeinit(&args);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		mArgumentsDeinit(&args);
		return 0;
	}

	/* graphicsOpts.fullscreen is true only if -f was actually given (its
	 * own init leaves it false) -- same "given" meaning amigaOpts.*Given
	 * fields track explicitly, just inherent to this field instead.
	 * graphicsOpts.multiplier is 0 if --scale/-1..-8 wasn't given, the
	 * same "not given" sentinel AMIGA_SETTING_SCALE already uses for
	 * "Auto". */
	struct AmigaSettings settings = {
		.fullscreen = graphicsOpts.fullscreen,
		.scale = graphicsOpts.multiplier,
		.ahiUnit = amigaOpts.ahiUnit,
		.muted = false,
	};
	/* Fills in any of fullscreen/scale/ahiUnit whose own CLI flag wasn't
	 * given (plus muted, which never has one), from whatever the
	 * Settings menu's "Save Settings" last wrote -- an explicit flag
	 * above always wins. Must run before VideoRTGInit()/AmigaAudioInit()
	 * below, which need the final decision already made. */
	_loadAmigaOpts(&settings, graphicsOpts.fullscreen, graphicsOpts.multiplier != 0, amigaOpts.ahiUnitGiven);

	if (!VideoRTGInit(settings.fullscreen, settings.scale)) {
		AmigaDebugLogf("Could not open an RTG screen/window (is Picasso96API.library installed?)\n");
		VideoRTGDeinit();
		mArgumentsDeinit(&args);
		return 1;
	}
	/* VideoRTGInit() may have silently upgraded a windowed request to
	 * fullscreen on its own (Workbench's own screen wasn't a suitable
	 * hicolor/truecolor mode to render into -- see its own comment).
	 * Resync this local copy with whatever it actually decided, since
	 * _runDirect()/_runBrowser() and every _loadRom()/VideoRTGReopen()
	 * call below thread `settings.fullscreen` through as the current
	 * state, not just the original request -- leaving it stale here
	 * would fight the real state on every later reopen (e.g. a ROM
	 * reload would keep passing "windowed" and force it back). */
	settings.fullscreen = VideoRTGFullscreen();

	/* Not fatal if this fails (missing/ancient lowlevel.library): keyboard
	 * input alone is still a fully working frontend. */
	GamepadInit();
	/* Likewise not fatal if ahi.device isn't installed -- a silent
	 * emulator still emulates. Must run before _runDirect()/_runBrowser()
	 * load any core, since AmigaAudioBindCore() (called from both) is a
	 * no-op without this having succeeded first. */
	AmigaAudioInit(settings.ahiUnit);

	int result = args.fname ? _runDirect(&args, settings.fullscreen, settings.scale, settings.ahiUnit, settings.muted) : _runBrowser();

	AmigaAudioDeinit();
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
