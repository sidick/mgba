/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-platform.h"

#include "audio-ahi.h"
#include "gamepad.h"
#include "gui-font-target.h"
#include "scale.h"
#include "video-rtg.h"

#include <mgba/core/core.h>
#include <mgba/core/input.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/gui.h>
#include <mgba-util/gui/font.h>

#include <stdlib.h>
#include <string.h>

/* FourCC-style platform ID for mInputMap/GUIInputKeys, following the
 * convention every other gui-runner.c platform uses (e.g. Wii's
 * GCN1_INPUT 0x47434E31 "GCN1") -- unique within this frontend, no
 * cross-platform meaning. */
#define AMIGA_KEYBOARD_INPUT 0x414D4B42 /* "AMKB" */

/* mgba's shared gui-runner.c menu layout (src/util/gui/menu.c) assumes
 * a screen wide enough that a left-aligned title and GUIDrawClock()'s
 * own horizontally-centered wall clock don't collide -- true on every
 * other gui-runner.c platform's own real screen (3DS 320-400px, PSP2
 * 960px, Switch 1280px, Wii 640px), but not on the GBA's native
 * GBA_VIDEO_HORIZONTAL_PIXELS (240) with this frontend's deliberately
 * oversized "2x" font (gui-font.c, chosen for legibility over the
 * original 1x bitmap font) -- confirmed live: title and clock text
 * visibly overlapped at native GBA resolution. An earlier version of
 * this file used a fixed 2x-GBA-resolution canvas scaled up to fit the
 * window; this one instead renders the menu at the window's own real,
 * native size (VideoRTGWindowContentSize(), video-rtg.h) -- the same
 * resolution every other gui-runner.c platform already works at,
 * crisp with no extra scale pass of its own -- and leaves the GBA
 * core's own native-resolution gameplay frame (s_coreBuffer) as the
 * only thing that still needs scaling, via _blitFaded()'s own
 * ScaleNearestFit() call. Entirely decoupled from VideoRTGBuffer()
 * either way. Resized (_resizeCanvas()) whenever the window's own size
 * changes -- at AmigaGuiPlatformInit() time and again from
 * _incrementScreenMode() after it reopens the window at a new scale.
 * Small windows (e.g. a 1x GB window, 160x144) can still be too narrow
 * for the menu layout to avoid the same title/clock collision -- a
 * known, accepted trade-off of rendering at the real window size rather
 * than enforcing an artificial minimum. */
static uint16_t* s_guiCanvas;
static int s_canvasWidth;
static int s_canvasHeight;

static void _resizeCanvas(struct mGUIRunner* runner) {
	int width, height;
	VideoRTGWindowContentSize(&width, &height);
	if (width == s_canvasWidth && height == s_canvasHeight && s_guiCanvas) {
		return;
	}

	uint16_t* canvas = calloc((size_t) width * height, sizeof(uint16_t));
	if (!canvas) {
		/* Keep the old (wrongly-sized, but real) canvas rather than
		 * leave s_guiCanvas NULL -- a stretched/cropped menu beats a
		 * crash from every other function here assuming it's non-NULL. */
		return;
	}
	free(s_guiCanvas);
	s_guiCanvas = canvas;
	s_canvasWidth = width;
	s_canvasHeight = height;

	GUIFontSetTarget(s_guiCanvas, s_canvasWidth, s_canvasHeight, s_canvasWidth);
	if (runner) {
		runner->params.width = s_canvasWidth;
		runner->params.height = s_canvasHeight;
	}
}

/* Set by _pollInput() (below), read by _running() -- see that pair's
 * own comments for why pumping the window's IDCMP queue belongs in
 * _pollInput() specifically. */
static bool s_shouldQuit;

/* The GBA core's own render target: deliberately NOT VideoRTGBuffer()
 * itself. _drawStart() clears VideoRTGBuffer() every frame (needed for
 * the menu's own redraw, which doesn't repaint pixels it no longer
 * needs), but during gameplay core->runFrame() happens *before*
 * drawStart() in gui-runner.c's own sequence -- rendering straight into
 * VideoRTGBuffer() would have drawStart() erase that frame before
 * _drawFrame() ever got to show it. Same AMIGA_VIDEO_STRIDE padding as
 * VideoRTGBuffer() itself (see its own comment) -- not required for
 * this private buffer's own sake, just one less thing to keep track of
 * separately. */
static uint16_t* s_coreBuffer;

static void _drawStart(void) {
	/* Erase the previous frame: unlike the GBA core (which always
	 * produces a full frame of its own), the menu only redraws the
	 * glyphs/icons it actually needs this frame, so stale content
	 * from a differently laid-out previous frame would otherwise
	 * remain on screen. */
	memset(s_guiCanvas, 0, (size_t) s_canvasWidth * s_canvasHeight * sizeof(uint16_t));
}

static void _drawEnd(void) {
	VideoRTGBlitCanvas(s_guiCanvas, s_canvasWidth, s_canvasHeight);
}

/* GUIPollInput() (mgba-util/gui.c) calls params.pollInput on every
 * single input poll in both the ROM browser and gameplay, making this
 * the one place guaranteed to run once per frame regardless of which
 * mode gui-runner.c is currently in -- so this is where
 * VideoRTGShouldQuit()'s IDCMP pump belongs (it's also what actually
 * updates VideoRTGGuiKeys()/VideoRTGKeys(), so skipping a frame here
 * would stall keyboard input too, not just quit detection). _running()
 * below just reads what this call found. */
static uint32_t _pollInput(const struct mInputMap* map) {
	s_shouldQuit = VideoRTGShouldQuit();
	return mInputMapKeyBits(map, AMIGA_KEYBOARD_INPUT, VideoRTGGuiKeys(), 0);
}

static enum GUICursorState _pollCursor(unsigned* x, unsigned* y) {
	/* No mouse/touch navigation yet -- keyboard only, same as the rest
	 * of this frontend. */
	UNUSED(x);
	UNUSED(y);
	return GUI_CURSOR_NOT_PRESENT;
}

static int _batteryState(void) {
	/* Desktop machine, no battery -- distinct from BATTERY_EMPTY, which
	 * would draw an (alarming) empty battery icon instead of none. */
	return BATTERY_NOT_PRESENT;
}

static void _guiPrepare(void) {
	/* No GPU batch to open (gui-font.c draws straight into the pixel
	 * buffer per call, see GUIFontSetTarget()'s own doc comment) --
	 * every other gui-runner.c platform uses this hook to bind a GL
	 * context/texture, which we have nothing to do for. */
}

static void _guiFinish(void) {
	/* Counterpart to _guiPrepare() above -- same reasoning. */
}

static enum GUIKeyboardStatus _getText(struct GUIKeyboardParams* keyboard) {
	/* No on-screen keyboard yet (ROM search/filename entry): honestly
	 * refuse rather than silently doing nothing to the caller's result
	 * buffer. */
	UNUSED(keyboard);
	return GUI_KEYBOARD_CANCEL;
}

/* mColor (COLOR_16_BIT + COLOR_5_6_5, see video-rtg.c's own comment on
 * this layout): R bits 0-4, G bits 5-10, B bits 11-15. Halves each
 * channel independently rather than shifting the whole 16-bit value, so
 * no channel bleeds into its neighbor's bits. Used for the pause menu's
 * "faded" backdrop, the same dimmed-screenshot-behind-the-menu effect
 * every other gui-runner.c platform gets from a GPU alpha blend. */
static uint16_t _fadeHalf(uint16_t v) {
	unsigned r = (v & 0x001F) >> 1;
	unsigned g = ((v >> 5) & 0x003F) >> 1;
	unsigned b = ((v >> 11) & 0x001F) >> 1;
	return (uint16_t) (r | (g << 5) | (b << 11));
}

/* Native-content-resolution staging area: faded (or not) here first,
 * then fit (ScaleNearestFit() -- s_guiCanvas is the window's own real
 * size now, not a fixed, conveniently-integer multiple of content size
 * the way the old fixed 2x canvas was) into s_guiCanvas. Fading at
 * native size first and letting the scale pass replicate already-faded
 * pixel values is simpler than fading the whole (possibly much larger)
 * canvas on every call. Sized for AMIGA_VIDEO_MAX_HEIGHT (video-rtg.h)
 * -- the same ceiling s_coreBuffer and VideoRTGBuffer() itself use --
 * since the loaded core's real content size (GBA_KEY_* 240x160, GB
 * 160x144, SGB-with-border 256x224) isn't known until this file's own
 * callers query it. */
static uint16_t s_frameStaging[AMIGA_VIDEO_STRIDE * AMIGA_VIDEO_MAX_HEIGHT];

/* Row-by-row rather than one flat copy: src can be padded to a stride
 * wider than `width` -- true for s_coreBuffer (AMIGA_VIDEO_STRIDE,
 * usually wider than the real content) but not for _drawScreenshot's
 * tightly-packed `pixels` (srcStride == width there). */
static void _blitFaded(const uint16_t* src, unsigned srcStride, unsigned width, unsigned height, bool faded) {
	unsigned y;
	for (y = 0; y < height; ++y) {
		const uint16_t* srcRow = &src[y * srcStride];
		uint16_t* stageRow = &s_frameStaging[y * AMIGA_VIDEO_STRIDE];
		if (!faded) {
			memcpy(stageRow, srcRow, width * sizeof(uint16_t));
			continue;
		}
		unsigned x;
		for (x = 0; x < width; ++x) {
			stageRow[x] = _fadeHalf(srcRow[x]);
		}
	}
	ScaleNearestFit(s_frameStaging, AMIGA_VIDEO_STRIDE, (int) width, (int) height,
		s_guiCanvas, s_canvasWidth, s_canvasWidth, s_canvasHeight);
}

static void _setup(struct mGUIRunner* runner) {
	if (!s_coreBuffer) {
		/* Same AMIGA_VIDEO_MAX_HEIGHT ceiling as s_frameStaging/
		 * VideoRTGBuffer() itself -- whatever core ends up loaded (GBA,
		 * GB, GBC) simply uses less of it. */
		s_coreBuffer = calloc((size_t) AMIGA_VIDEO_STRIDE * AMIGA_VIDEO_MAX_HEIGHT, sizeof(uint16_t));
	}
	/* core->setVideoBuffer(), not VideoRTGBuffer() -- see s_coreBuffer's
	 * own comment above for why the two must stay separate. Must run
	 * before gui-runner.c's own core->reset() call right after this
	 * returns -- same constraint main.c's _bindVideoBuffer() documents
	 * (src/gb/core.c's own reset only associates its video renderer with
	 * a buffer that's already bound). gui-runner.c already calls setup()
	 * before reset() in its own sequence, so no extra ordering care is
	 * needed here beyond that existing contract. */
	runner->core->setVideoBuffer(runner->core, s_coreBuffer, AMIGA_VIDEO_STRIDE);
	AmigaAudioBindCore(runner->core);
}

static void _drawFrame(struct mGUIRunner* runner, bool faded) {
	/* Real content size, not a hardcoded GBA assumption -- same
	 * currentVideoSize() main.c's own _sizeForCore() uses, and same
	 * caveat: only answers correctly once core->reset() has actually
	 * run (see that function's own comment) -- true here, since
	 * drawFrame() is only ever called from gui-runner.c's main gameplay
	 * loop, well after setup()+reset(). */
	unsigned width, height;
	runner->core->currentVideoSize(runner->core, &width, &height);
	_blitFaded(s_coreBuffer, AMIGA_VIDEO_STRIDE, width, height, faded);
}

static void _drawScreenshot(struct mGUIRunner* runner, const mColor* pixels, unsigned width, unsigned height, bool faded) {
	UNUSED(runner);
	/* A save state's stored screenshot -- same mColor pixel type as
	 * s_coreBuffer, just not padded to AMIGA_VIDEO_STRIDE (exactly
	 * width*height, the core's own content size at the time it was
	 * saved -- width/height are given explicitly here, not assumed). */
	_blitFaded(pixels, width, width, height, faded);
}

static uint16_t _pollGameInput(struct mGUIRunner* runner) {
	/* Already up to date for this frame: GUIPollInput() (and so
	 * _pollInput() above) always runs before gui-runner.c reads
	 * pollGameInput() in its own per-frame sequence. Same keyboard+
	 * gamepad GBA_KEY_* bitmask this frontend's non-GUI main loop
	 * already uses. */
	UNUSED(runner);
	return (uint16_t) (VideoRTGKeys() | GamepadKeys());
}

static bool _running(struct mGUIRunner* runner) {
	UNUSED(runner);
	return !s_shouldQuit;
}

/* Cycles 1x -> 2x -> 3x -> 1x, reusing VideoRTGReopen() exactly as
 * menu.c's MENU_ACTION_SCALE_* cases do (src/platform/amiga/main.c),
 * just driven from gui-runner.c's own mGUI_INPUT_SCREEN_MODE input
 * instead of a pulldown pick. Doesn't include Auto: VideoRTGScale()
 * reports the real resolved scale even after an auto-fit (never 0), so
 * there's no way to tell "currently auto-fit to N" apart from
 * "currently manually N" to cycle Auto back in correctly -- Auto stays
 * reachable from the native Settings menu, which doesn't have that
 * ambiguity (it tracks its own last-picked item). */
static void _incrementScreenMode(struct mGUIRunner* runner) {
	int current = VideoRTGScale();
	int next = (current >= 1 && current <= 3) ? (current % 3) + 1 : 1;
	VideoRTGReopen(VideoRTGFullscreen(), next);
	/* The window just changed size -- resize the GUI canvas to match, so
	 * the pause overlay's next redraw uses the new size too, not a
	 * stretched/cropped stale one. */
	_resizeCanvas(runner);
}

static void _teardown(struct mGUIRunner* runner) {
	AmigaGuiPlatformDeinit(runner);
	free(s_coreBuffer);
	s_coreBuffer = NULL;
}

static struct GUIInputKeys s_keySources[] = {
	{
		.name = "Amiga Keyboard",
		.id = AMIGA_KEYBOARD_INPUT,
		.keyNames = (const char*[]) {
			"Up", "Down", "Left", "Right", "Select", "Back", "Cancel",
		},
		.nKeys = 7,
	},
	{ 0 },
};

void AmigaGuiPlatformInit(struct mGUIRunner* runner) {
	_resizeCanvas(runner);

	runner->params.font = GUIFontCreate();
	/* Empty, not "SYS:" -- AmigaDOS has no single filesystem root the
	 * way POSIX has "/": SYS:, a game's own MGBA: host mount, RAM:, and
	 * any other mounted volume all sit side by side with nothing above
	 * them. VDirOpen("") (vfs-dirent.c's own __amigaos__ branch) returns
	 * a real "every mounted volume" listing for this case, the same way
	 * Wii's VDeviceList() already stands in for a root on a system with
	 * multiple memory-card/USB mounts -- so basePath being empty here
	 * means "Up" from any one volume's own top level (e.g. SYS:) lands
	 * back on that volume list instead of being stuck with nowhere to
	 * go, which hardcoding one specific volume as basePath otherwise
	 * leaves no way to reach. */
	runner->params.basePath = "";
	runner->params.drawStart = _drawStart;
	runner->params.drawEnd = _drawEnd;
	runner->params.pollInput = _pollInput;
	runner->params.pollCursor = _pollCursor;
	runner->params.batteryState = _batteryState;
	runner->params.guiPrepare = _guiPrepare;
	runner->params.guiFinish = _guiFinish;
	runner->params.getText = _getText;
	/* mGUIInit()'s own GUIInit(&runner->params) call (gui-runner.c) ran
	 * before AmigaGuiPlatformInit() was reached -- GUIInit() copies
	 * basePath into currentPath once, clears inputHistory, otherwise a
	 * no-op given basePath is already "" by this struct's own zero-init.
	 * Kept for the same reason the font/path fields above are still set
	 * explicitly here rather than relied on being zeroed: this function,
	 * not main.c's struct literal, is the one place that actually owns
	 * what the ROM browser starts from. */
	GUIInit(&runner->params);

	/* mGUIInit() (gui-runner.c) already called mInputMapInit() on this
	 * before AmigaGuiPlatformInit() is reached -- see this file's own
	 * header comment on call order. Bit positions here are enum
	 * AmigaGuiKey (video-rtg.h), the same ones _pollInput() reads via
	 * VideoRTGGuiKeys(). */
	mInputBindKey(&runner->params.keyMap, AMIGA_KEYBOARD_INPUT, AMIGA_GUI_KEY_UP, GUI_INPUT_UP);
	mInputBindKey(&runner->params.keyMap, AMIGA_KEYBOARD_INPUT, AMIGA_GUI_KEY_DOWN, GUI_INPUT_DOWN);
	mInputBindKey(&runner->params.keyMap, AMIGA_KEYBOARD_INPUT, AMIGA_GUI_KEY_LEFT, GUI_INPUT_LEFT);
	mInputBindKey(&runner->params.keyMap, AMIGA_KEYBOARD_INPUT, AMIGA_GUI_KEY_RIGHT, GUI_INPUT_RIGHT);
	mInputBindKey(&runner->params.keyMap, AMIGA_KEYBOARD_INPUT, AMIGA_GUI_KEY_SELECT, GUI_INPUT_SELECT);
	mInputBindKey(&runner->params.keyMap, AMIGA_KEYBOARD_INPUT, AMIGA_GUI_KEY_BACK, GUI_INPUT_BACK);
	mInputBindKey(&runner->params.keyMap, AMIGA_KEYBOARD_INPUT, AMIGA_GUI_KEY_CANCEL, GUI_INPUT_CANCEL);

	runner->keySources = s_keySources;

	/* The rest of the mGUIRunner callback set (gui-runner.h) -- setup
	 * binds the core's own render target (s_coreBuffer, not
	 * VideoRTGBuffer() -- see its own comment); gameLoaded/gameUnloaded/
	 * prepareForFrame/paused/unpaused/setFrameLimiter are left NULL
	 * (all individually optional, `if (runner->x)` guarded in
	 * gui-runner.c): nothing peripheral to enable per game, no texture
	 * double-buffering to flip, nothing special needed across a pause,
	 * and no frame-limiting mechanism exists anywhere in this frontend
	 * yet to hook fast-forward into. */
	runner->setup = _setup;
	runner->teardown = _teardown;
	runner->drawFrame = _drawFrame;
	runner->drawScreenshot = _drawScreenshot;
	runner->pollGameInput = _pollGameInput;
	runner->running = _running;
	runner->incrementScreenMode = _incrementScreenMode;
}

void AmigaGuiPlatformDeinit(struct mGUIRunner* runner) {
	GUIFontDestroy(runner->params.font);
	runner->params.font = NULL;
	free(s_guiCanvas);
	s_guiCanvas = NULL;
	s_canvasWidth = 0;
	s_canvasHeight = 0;
}
