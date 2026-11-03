/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* Standalone visual check for gui-font.c -- not part of the real
 * frontend (main.c). Opens the RTG window/screen exactly like main.c
 * does, points GUIFontSetTarget() at the same native GBA-sized buffer
 * video-rtg.c already knows how to blit, draws some text and icons once,
 * then idles until the window closes. Lets gui-font.c be verified the
 * same way every other piece of this frontend has been this session --
 * a Copperline screenshot -- without needing the full gui-runner.c menu
 * wired up first. */
#include "gui-font-target.h"
#include "video-rtg.h"

#include <mgba/gba/interface.h>
#include <mgba-util/gui/font.h>

#include <stdio.h>

int main(int argc, char** argv) {
	bool fullscreen = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'F';

	if (!VideoRTGInit(fullscreen, 0)) {
		fprintf(stderr, "Could not open an RTG screen/window\n");
		VideoRTGDeinit();
		return 1;
	}

	struct GUIFont* font = GUIFontCreate();
	if (!font) {
		fprintf(stderr, "Could not decode the embedded font/icon assets\n");
		VideoRTGDeinit();
		return 1;
	}

	GUIFontSetTarget(VideoRTGBuffer(), GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS, AMIGA_VIDEO_STRIDE);

	GUIFontPrint(font, 4, GUIFontHeight(font), GUI_ALIGN_LEFT, 0xFFFFFFFF, "mGBA Amiga");
	GUIFontPrint(font, 4, GUIFontHeight(font) * 2 + 4, GUI_ALIGN_LEFT, 0xFFFF00FF, "gui-font.c test");
	GUIFontPrintf(font, GBA_VIDEO_HORIZONTAL_PIXELS - 4, GUIFontHeight(font) * 3 + 8, GUI_ALIGN_RIGHT, 0x7FFFFFFF, "%d x %d", GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);

	GUIFontDrawIcon(font, 4, GBA_VIDEO_VERTICAL_PIXELS - 4, GUI_ALIGN_LEFT | GUI_ALIGN_BOTTOM, GUI_ORIENT_0, 0xFFFFFFFF, GUI_ICON_BATTERY_FULL);
	GUIFontDrawIcon(font, 40, GBA_VIDEO_VERTICAL_PIXELS - 4, GUI_ALIGN_LEFT | GUI_ALIGN_BOTTOM, GUI_ORIENT_0, 0xFF00FFFF, GUI_ICON_CURSOR);
	GUIFontDrawIconSize(font, 70, GBA_VIDEO_VERTICAL_PIXELS - 36, 32, 32, 0xFFFFFFFF, GUI_ICON_POINTER);

	VideoRTGBlit();

	bool quit = false;
	while (!quit) {
		quit = VideoRTGShouldQuit();
	}

	GUIFontDestroy(font);
	VideoRTGDeinit();
	return 0;
}
