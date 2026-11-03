/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "video-rtg.h"

#include <mgba/gba/interface.h>

#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/Picasso96.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>

#include <stdlib.h>

/* The P96 inline stubs (inline/Picasso96.h, pulled in via proto/Picasso96.h)
 * reference this exact global name -- it MUST be called P96Base. */
struct Library* P96Base;

static struct Screen* s_screen;
static struct Window* s_window;
static uint16_t* s_buffer;
static uint16_t* s_swapped;

/* mgba's mColor, built with COLOR_16_BIT + COLOR_5_6_5 (see
 * src/platform/amiga/CMakeLists.txt), is a plain uint16_t with (per
 * include/mgba-util/image.h) R in bits 0-4, G in bits 5-10, B in bits
 * 11-15 -- i.e. the *logical* value is B-high/G-mid/R-low. Stored natively
 * on this big-endian CPU, that's bit-for-bit identical to Picasso96's
 * RGBFB_B5G6R5PC *after* a 16-bit byte swap: P96's "PC" (little-endian/VGA
 * convention) formats document their layout as the two bytes in memory
 * order (low address first), e.g. B5G6R5PC's "gggrrrrrbbbbbggg" is
 * high-byte="bbbbbggg" + low-byte="gggrrrrr" stored low-address-first. Our
 * native big-endian store of the same logical value puts the numerically
 * high byte ("bbbbbggg") at the LOWER address already -- i.e. the opposite
 * physical byte order PC format wants. Swapping each 16-bit value's bytes
 * before handing them to p96WritePixelArray converts between the two.
 *
 * Verified correct, not just by bit-level derivation: booted under
 * Copperline (AmigaOS 3.2.2, Z3660 RTG, 16-bit screen) running one of
 * mgba's own cinema test ROMs (cinema/gba/obj/2d-wrap), the rendered
 * window matched that test's golden reference
 * (cinema/gba/obj/2d-wrap/baseline_0000.png) exactly -- same two
 * white-left/green-right sprites, same positions, no channel swap or
 * scrambling. */
static void _blitSwapBytes(void) {
	size_t i;
	for (i = 0; i < AMIGA_VIDEO_STRIDE * GBA_VIDEO_VERTICAL_PIXELS; ++i) {
		uint16_t v = s_buffer[i];
		s_swapped[i] = (uint16_t) ((v >> 8) | (v << 8));
	}
}

bool VideoRTGInit(bool fullscreen) {
	s_buffer = calloc(AMIGA_VIDEO_STRIDE * GBA_VIDEO_VERTICAL_PIXELS, sizeof(uint16_t));
	s_swapped = calloc(AMIGA_VIDEO_STRIDE * GBA_VIDEO_VERTICAL_PIXELS, sizeof(uint16_t));
	if (!s_buffer || !s_swapped) {
		return false;
	}

	P96Base = OpenLibrary(P96NAME, 2);
	if (!P96Base) {
		return false;
	}

	if (fullscreen) {
		ULONG id = p96BestModeIDTags(
			P96BIDTAG_NominalWidth, GBA_VIDEO_HORIZONTAL_PIXELS,
			P96BIDTAG_NominalHeight, GBA_VIDEO_VERTICAL_PIXELS,
			P96BIDTAG_Depth, 16,
			P96BIDTAG_FormatsAllowed, RGBFF_HICOLOR | RGBFF_TRUECOLOR,
			TAG_DONE);
		if (id == (ULONG) INVALID_ID) {
			return false;
		}

		s_screen = p96OpenScreenTags(
			P96SA_DisplayID, id,
			P96SA_Width, GBA_VIDEO_HORIZONTAL_PIXELS,
			P96SA_Height, GBA_VIDEO_VERTICAL_PIXELS,
			P96SA_Depth, 16,
			P96SA_Title, (ULONG) "mGBA",
			P96SA_Quiet, TRUE,
			TAG_DONE);
		if (!s_screen) {
			return false;
		}

		s_window = OpenWindowTags(NULL,
			WA_CustomScreen, (ULONG) s_screen,
			WA_Backdrop, TRUE,
			WA_Borderless, TRUE,
			WA_Activate, TRUE,
			WA_RMBTrap, TRUE,
			WA_Width, GBA_VIDEO_HORIZONTAL_PIXELS,
			WA_Height, GBA_VIDEO_VERTICAL_PIXELS,
			WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY,
			TAG_DONE);
	} else {
		s_window = OpenWindowTags(NULL,
			WA_PubScreenName, (ULONG) "Workbench",
			WA_Title, (ULONG) "mGBA",
			WA_InnerWidth, GBA_VIDEO_HORIZONTAL_PIXELS,
			WA_InnerHeight, GBA_VIDEO_VERTICAL_PIXELS,
			WA_DragBar, TRUE,
			WA_DepthGadget, TRUE,
			WA_CloseGadget, TRUE,
			WA_Activate, TRUE,
			WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY,
			TAG_DONE);
	}

	return s_window != NULL;
}

uint16_t* VideoRTGBuffer(void) {
	return s_buffer;
}

void VideoRTGBlit(void) {
	if (!s_window) {
		return;
	}
	_blitSwapBytes();

	struct RenderInfo ri;
	ri.Memory = s_swapped;
	ri.BytesPerRow = AMIGA_VIDEO_STRIDE * sizeof(uint16_t);
	ri.RGBFormat = RGBFB_B5G6R5PC;

	p96WritePixelArray(&ri, 0, 0, s_window->RPort, s_window->BorderLeft, s_window->BorderTop,
		GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
}

bool VideoRTGShouldQuit(void) {
	if (!s_window) {
		return true;
	}
	bool quit = false;
	struct IntuiMessage* msg;
	while ((msg = (struct IntuiMessage*) GetMsg(s_window->UserPort))) {
		if (msg->Class == IDCMP_CLOSEWINDOW) {
			quit = true;
		}
		ReplyMsg((struct Message*) msg);
	}
	return quit;
}

void VideoRTGDeinit(void) {
	if (s_window) {
		CloseWindow(s_window);
		s_window = NULL;
	}
	if (s_screen) {
		/* Never CloseScreen() a p96-opened screen -- must be p96CloseScreen(). */
		p96CloseScreen(s_screen);
		s_screen = NULL;
	}
	if (P96Base) {
		CloseLibrary(P96Base);
		P96Base = NULL;
	}
	free(s_buffer);
	free(s_swapped);
	s_buffer = NULL;
	s_swapped = NULL;
}
