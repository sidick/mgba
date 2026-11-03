/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-platform.h"

#include "gui-font-target.h"
#include "video-rtg.h"

#include <mgba/core/input.h>
#include <mgba/gba/interface.h>
#include <mgba-util/gui.h>
#include <mgba-util/gui/font.h>

#include <string.h>

/* FourCC-style platform ID for mInputMap/GUIInputKeys, following the
 * convention every other gui-runner.c platform uses (e.g. Wii's
 * GCN1_INPUT 0x47434E31 "GCN1") -- unique within this frontend, no
 * cross-platform meaning. */
#define AMIGA_KEYBOARD_INPUT 0x414D4B42 /* "AMKB" */

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

static uint32_t _pollInput(const struct mInputMap* map) {
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
}

void AmigaGuiPlatformDeinit(struct mGUIRunner* runner) {
	GUIFontDestroy(runner->params.font);
	runner->params.font = NULL;
}
