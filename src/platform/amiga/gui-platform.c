/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-platform.h"

#include "gamepad.h"
#include "gui-font-target.h"
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
	memset(VideoRTGBuffer(), 0, AMIGA_VIDEO_STRIDE * GBA_VIDEO_VERTICAL_PIXELS * sizeof(uint16_t));
}

static void _drawEnd(void) {
	VideoRTGBlit();
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

/* Row-by-row rather than one flat copy: src and VideoRTGBuffer() can
 * each be padded to AMIGA_VIDEO_STRIDE but srcWidth (the real content
 * width) need not fill that stride -- true for s_coreBuffer (always
 * GBA_VIDEO_HORIZONTAL_PIXELS of real content) and for _drawScreenshot's
 * arbitrary width/height alike. */
static void _blitFaded(const uint16_t* src, unsigned srcStride, unsigned width, unsigned height, bool faded) {
	uint16_t* dst = VideoRTGBuffer();
	unsigned y;
	for (y = 0; y < height; ++y) {
		const uint16_t* srcRow = &src[y * srcStride];
		uint16_t* dstRow = &dst[y * AMIGA_VIDEO_STRIDE];
		if (!faded) {
			memcpy(dstRow, srcRow, width * sizeof(uint16_t));
			continue;
		}
		unsigned x;
		for (x = 0; x < width; ++x) {
			dstRow[x] = _fadeHalf(srcRow[x]);
		}
	}
}

static void _setup(struct mGUIRunner* runner) {
	if (!s_coreBuffer) {
		s_coreBuffer = calloc(AMIGA_VIDEO_STRIDE * GBA_VIDEO_VERTICAL_PIXELS, sizeof(uint16_t));
	}
	/* core->setVideoBuffer(), not VideoRTGBuffer() -- see s_coreBuffer's
	 * own comment above for why the two must stay separate. */
	runner->core->setVideoBuffer(runner->core, s_coreBuffer, AMIGA_VIDEO_STRIDE);
}

static void _drawFrame(struct mGUIRunner* runner, bool faded) {
	UNUSED(runner);
	_blitFaded(s_coreBuffer, AMIGA_VIDEO_STRIDE, GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS, faded);
}

static void _drawScreenshot(struct mGUIRunner* runner, const mColor* pixels, unsigned width, unsigned height, bool faded) {
	UNUSED(runner);
	/* A save state's stored screenshot -- same mColor pixel type as
	 * s_coreBuffer, just not padded to AMIGA_VIDEO_STRIDE (it's exactly
	 * width*height, mCoreLoadState's own GBA_VIDEO_HORIZONTAL_PIXELS). */
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
	UNUSED(runner);
	int current = VideoRTGScale();
	int next = (current >= 1 && current <= 3) ? (current % 3) + 1 : 1;
	VideoRTGReopen(VideoRTGFullscreen(), next);
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
	GUIFontSetTarget(VideoRTGBuffer(), GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS, AMIGA_VIDEO_STRIDE);

	runner->params.width = GBA_VIDEO_HORIZONTAL_PIXELS;
	runner->params.height = GBA_VIDEO_VERTICAL_PIXELS;
	runner->params.font = GUIFontCreate();
	runner->params.basePath = "";
	runner->params.drawStart = _drawStart;
	runner->params.drawEnd = _drawEnd;
	runner->params.pollInput = _pollInput;
	runner->params.pollCursor = _pollCursor;
	runner->params.batteryState = _batteryState;
	runner->params.guiPrepare = _guiPrepare;
	runner->params.guiFinish = _guiFinish;
	runner->params.getText = _getText;

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
}
