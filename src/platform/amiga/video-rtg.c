/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "menu.h"
#include "scale.h"
#include "video-rtg.h"

#include <mgba/gba/interface.h>
#include <mgba/internal/gba/input.h>

#include <devices/inputevent.h>
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
static uint16_t* s_scaledOutput;
static int s_scale;
static int s_blitX;
static int s_blitY;
static bool s_fullscreen;
static uint32_t s_keys;
static uint32_t s_guiKeys;
static enum MenuAction s_menuAction;

/* VideoRTGBlitCanvas()'s own scratch buffers (gui-platform.c's logical
 * canvas, a fixed size unrelated to GBA_VIDEO_HORIZONTAL/VERTICAL_PIXELS
 * -- see that function's own comment) -- allocated lazily on first use,
 * grown (never shrunk) if a caller ever asks for a larger canvas than
 * before. Not performance-critical the way the gameplay blit is (GUI
 * redraws, not 60fps-every-frame gameplay), so no need to pre-size these
 * at VideoRTGInit() the way s_buffer/s_swapped are. */
static uint16_t* s_canvasSwapped;
static int s_canvasSwappedCapacity;
static uint16_t* s_canvasScaledOutput;
static int s_canvasScaledCapacity;

/* Keyboard-only for now (no joystick/gamepad support yet -- see
 * video-rtg.h's VideoRTGKeys() comment): raw key codes below are the
 * standard Amiga keyboard scancodes (devices/rawkeycodes.h isn't shipped
 * with this NDK install, hence the literals -- these are long-stable
 * hardware values, not toolchain-specific). Mirrors mGBA's own desktop
 * (Qt/SDL) default mapping: X/Z for A/B, A/S for L/R, Return/Backspace
 * for Start/Select. */
static int _mapRawKey(UWORD code) {
	switch (code) {
	case 0x4C: return GBA_KEY_UP;
	case 0x4D: return GBA_KEY_DOWN;
	case 0x4E: return GBA_KEY_RIGHT;
	case 0x4F: return GBA_KEY_LEFT;
	case 0x32: return GBA_KEY_A;      /* X */
	case 0x31: return GBA_KEY_B;      /* Z */
	case 0x20: return GBA_KEY_L;      /* A */
	case 0x21: return GBA_KEY_R;      /* S */
	case 0x44: return GBA_KEY_START;  /* Return */
	case 0x41: return GBA_KEY_SELECT; /* Backspace */
	default: return GBA_KEY_NONE;
	}
}

/* Bit positions are enum AmigaGuiKey (video-rtg.h), shared with
 * gui-platform.c so it can bind the same symbolic constants. */
static int _mapRawKeyGui(UWORD code) {
	switch (code) {
	case 0x4C: return AMIGA_GUI_KEY_UP;
	case 0x4D: return AMIGA_GUI_KEY_DOWN;
	case 0x4E: return AMIGA_GUI_KEY_RIGHT;
	case 0x4F: return AMIGA_GUI_KEY_LEFT;
	case 0x44: return AMIGA_GUI_KEY_SELECT; /* Return */
	case 0x41: return AMIGA_GUI_KEY_BACK;   /* Backspace */
	case 0x45: return AMIGA_GUI_KEY_CANCEL; /* Esc */
	default: return -1;
	}
}

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

/* Enumerates real display modes (not the synthetic "closest fit" a
 * p96BestModeIDTags nominal-size request can fabricate on some drivers --
 * seen in practice on Z3660, which happily invents a CUSTOM mode of
 * whatever exact size is asked for, rather than reporting back what the
 * board/monitor can actually do) and returns the one with the largest
 * pixel area, plus the largest integer GBA-resolution scale that fits
 * inside it. Returns false if P96 reports no usable mode at all. */
static bool _pickAutoScaleMode(ULONG* outId, int* outScale) {
	struct List* modes = p96AllocModeListTags(
		P96MA_MinWidth, GBA_VIDEO_HORIZONTAL_PIXELS,
		P96MA_MinHeight, GBA_VIDEO_VERTICAL_PIXELS,
		P96MA_MinDepth, 16,
		P96MA_FormatsAllowed, RGBFF_HICOLOR | RGBFF_TRUECOLOR,
		TAG_DONE);
	if (!modes) {
		return false;
	}

	bool found = false;
	ULONG bestId = 0;
	UWORD bestWidth = 0;
	UWORD bestHeight = 0;
	struct P96Mode* mode;
	for (mode = (struct P96Mode*) modes->lh_Head; mode->Node.ln_Succ; mode = (struct P96Mode*) mode->Node.ln_Succ) {
		if ((ULONG) mode->Width * mode->Height > (ULONG) bestWidth * bestHeight) {
			bestId = mode->DisplayID;
			bestWidth = mode->Width;
			bestHeight = mode->Height;
			found = true;
		}
	}
	p96FreeModeList(modes);

	if (!found) {
		return false;
	}
	*outId = bestId;
	*outScale = ScaleBestFit(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS, bestWidth, bestHeight);
	return true;
}

bool VideoRTGInit(bool fullscreen, int scale) {
	s_keys = 0;
	/* Allocated once for the whole process lifetime, not torn down by
	 * VideoRTGReopen() -- core->setVideoBuffer() is only ever called once
	 * (see main.c), so a later live scale/mode change (the Settings menu)
	 * must keep this same pointer valid, only reopening the window/screen
	 * around it. */
	s_buffer = calloc(AMIGA_VIDEO_STRIDE * GBA_VIDEO_VERTICAL_PIXELS, sizeof(uint16_t));
	s_swapped = calloc(AMIGA_VIDEO_STRIDE * GBA_VIDEO_VERTICAL_PIXELS, sizeof(uint16_t));
	if (!s_buffer || !s_swapped) {
		return false;
	}

	P96Base = OpenLibrary(P96NAME, 2);
	if (!P96Base) {
		return false;
	}

	return VideoRTGReopen(fullscreen, scale);
}

bool VideoRTGReopen(bool fullscreen, int scale) {
	/* Tear down whatever window/screen/menu/scaled-output the previous
	 * VideoRTGInit()/VideoRTGReopen() call made -- but not s_buffer/
	 * s_swapped/P96Base, which outlive any number of reopens. Safe to
	 * call when there's nothing open yet (VideoRTGInit()'s own first
	 * call): every pointer here starts NULL. */
	MenuDeinit(s_window);
	if (s_window) {
		CloseWindow(s_window);
		s_window = NULL;
	}
	if (s_screen) {
		p96CloseScreen(s_screen);
		s_screen = NULL;
	}
	free(s_scaledOutput);
	s_scaledOutput = NULL;

	/* scale == 0 means "auto": fullscreen picks the largest integer fit
	 * for the best available mode (resolved below); windowed has no
	 * screen of its own to measure, so auto there just means the
	 * previous fixed default of 1. */
	bool autoScale = fullscreen && scale == 0;
	if (scale < 1) {
		scale = 1;
	}
	s_fullscreen = fullscreen;

	if (fullscreen) {
		ULONG id;
		if (autoScale) {
			if (!_pickAutoScaleMode(&id, &scale)) {
				return false;
			}
		} else {
			id = p96BestModeIDTags(
				P96BIDTAG_NominalWidth, GBA_VIDEO_HORIZONTAL_PIXELS * scale,
				P96BIDTAG_NominalHeight, GBA_VIDEO_VERTICAL_PIXELS * scale,
				P96BIDTAG_Depth, 16,
				P96BIDTAG_FormatsAllowed, RGBFF_HICOLOR | RGBFF_TRUECOLOR,
				TAG_DONE);
			if (id == (ULONG) INVALID_ID) {
				return false;
			}
		}

		s_scale = scale;
		unsigned scaledWidth = GBA_VIDEO_HORIZONTAL_PIXELS * s_scale;
		unsigned scaledHeight = GBA_VIDEO_VERTICAL_PIXELS * s_scale;
		if (s_scale > 1) {
			/* Owned entirely by us (not core->setVideoBuffer()'s stride
			 * convention), so no AMIGA_VIDEO_STRIDE-style padding needed
			 * -- exactly scaledWidth pixels per row. */
			s_scaledOutput = calloc((size_t) scaledWidth * scaledHeight, sizeof(uint16_t));
			if (!s_scaledOutput) {
				return false;
			}
		}

		/* The chosen mode's own real geometry, not necessarily equal to
		 * scaledWidth/scaledHeight -- an explicit -S (or an auto-picked
		 * mode scale doesn't divide evenly into) can leave real margin.
		 * Opening the screen/window at the mode's full size and blitting
		 * our content centered within it (below) means that margin reads
		 * as a deliberate black border around the picture rather than an
		 * arbitrary unfilled corner. */
		unsigned screenWidth = (unsigned) p96GetModeIDAttr(id, P96IDA_WIDTH);
		unsigned screenHeight = (unsigned) p96GetModeIDAttr(id, P96IDA_HEIGHT);
		if (screenWidth < scaledWidth) {
			screenWidth = scaledWidth;
		}
		if (screenHeight < scaledHeight) {
			screenHeight = scaledHeight;
		}
		s_blitX = (int) ((screenWidth - scaledWidth) / 2);
		s_blitY = (int) ((screenHeight - scaledHeight) / 2);

		s_screen = p96OpenScreenTags(
			P96SA_DisplayID, id,
			P96SA_Width, screenWidth,
			P96SA_Height, screenHeight,
			P96SA_Depth, 16,
			P96SA_Title, (ULONG) "mGBA",
			P96SA_Quiet, TRUE,
			TAG_DONE);
		if (!s_screen) {
			return false;
		}

		/* No WA_RMBTrap: that flag steals the right mouse button as a
		 * plain IDCMP_MOUSEBUTTONS event before Intuition's own menu-
		 * pulldown handling ever sees it, breaking the Settings/Project
		 * menu below outright. We don't use RMB for anything else. */
		s_window = OpenWindowTags(NULL,
			WA_CustomScreen, (ULONG) s_screen,
			WA_Backdrop, TRUE,
			WA_Borderless, TRUE,
			WA_Activate, TRUE,
			WA_Width, screenWidth,
			WA_Height, screenHeight,
			WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_MENUPICK,
			TAG_DONE);
		if (s_window && (s_blitX > 0 || s_blitY > 0)) {
			/* Plain black for now -- P96's own default background
			 * otherwise shows through the margin left by a scale that
			 * doesn't evenly fill the mode. A bevel/border here is a
			 * natural follow-up, not done yet. Filled once: our own
			 * per-frame blit never touches anything outside the centered
			 * content rect, so this never needs redrawing. */
			p96RectFill(s_window->RPort, 0, 0, (LONG) screenWidth - 1, (LONG) screenHeight - 1, 0xFF000000);
		}
	} else {
		s_scale = scale;
		unsigned scaledWidth = GBA_VIDEO_HORIZONTAL_PIXELS * s_scale;
		unsigned scaledHeight = GBA_VIDEO_VERTICAL_PIXELS * s_scale;
		if (s_scale > 1) {
			s_scaledOutput = calloc((size_t) scaledWidth * scaledHeight, sizeof(uint16_t));
			if (!s_scaledOutput) {
				return false;
			}
		}

		/* No screen margin to center within -- the window is always
		 * opened at exactly the content size. */
		s_blitX = 0;
		s_blitY = 0;

		s_window = OpenWindowTags(NULL,
			WA_PubScreenName, (ULONG) "Workbench",
			WA_Title, (ULONG) "mGBA",
			WA_InnerWidth, scaledWidth,
			WA_InnerHeight, scaledHeight,
			WA_DragBar, TRUE,
			WA_DepthGadget, TRUE,
			WA_CloseGadget, TRUE,
			WA_Activate, TRUE,
			WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_MENUPICK,
			TAG_DONE);
	}

	if (!s_window) {
		return false;
	}

	/* Not fatal if this fails (missing/ancient gadtools.library): the
	 * window still works, just without a menu. MenuInit()'s "currentScale"
	 * is 0 for "Auto" specifically (not whatever numeric scale auto-detect
	 * happened to resolve to, which may not even be one of the menu's
	 * 1x/2x/3x options) -- windowed mode is never auto (see the comment
	 * above), so `scale` there is already the right value either way. */
	MenuInit(s_window, (fullscreen && autoScale) ? 0 : scale);
	return true;
}

uint16_t* VideoRTGBuffer(void) {
	return s_buffer;
}

void VideoRTGBlit(void) {
	if (!s_window) {
		return;
	}
	/* Byte-swap at native size first, scale (if any) second -- scaling
	 * is pure pixel replication, indifferent to byte order, and this
	 * way the swap pass touches GBA_VIDEO_HORIZONTAL_PIXELS *
	 * GBA_VIDEO_VERTICAL_PIXELS values regardless of scale instead of
	 * scale^2 as many. */
	_blitSwapBytes();

	struct RenderInfo ri;
	ri.RGBFormat = RGBFB_B5G6R5PC;

	if (s_scale > 1) {
		unsigned scaledWidth = GBA_VIDEO_HORIZONTAL_PIXELS * s_scale;
		ScaleNearest(s_swapped, AMIGA_VIDEO_STRIDE, GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS,
			s_scaledOutput, scaledWidth, s_scale);
		ri.Memory = s_scaledOutput;
		ri.BytesPerRow = scaledWidth * sizeof(uint16_t);
		p96WritePixelArray(&ri, 0, 0, s_window->RPort, s_window->BorderLeft + s_blitX, s_window->BorderTop + s_blitY,
			scaledWidth, GBA_VIDEO_VERTICAL_PIXELS * s_scale);
	} else {
		ri.Memory = s_swapped;
		ri.BytesPerRow = AMIGA_VIDEO_STRIDE * sizeof(uint16_t);
		p96WritePixelArray(&ri, 0, 0, s_window->RPort, s_window->BorderLeft + s_blitX, s_window->BorderTop + s_blitY,
			GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
	}
}

void VideoRTGBlitCanvas(const uint16_t* canvas, int canvasWidth, int canvasHeight) {
	if (!s_window) {
		return;
	}

	size_t canvasPixels = (size_t) canvasWidth * canvasHeight;
	if (s_canvasSwappedCapacity < (int) canvasPixels) {
		free(s_canvasSwapped);
		s_canvasSwapped = malloc(canvasPixels * sizeof(uint16_t));
		s_canvasSwappedCapacity = s_canvasSwapped ? (int) canvasPixels : 0;
	}
	if (!s_canvasSwapped) {
		return;
	}
	size_t i;
	for (i = 0; i < canvasPixels; ++i) {
		uint16_t v = canvas[i];
		s_canvasSwapped[i] = (uint16_t) ((v >> 8) | (v << 8));
	}

	/* Fit the canvas to whatever content size VideoRTGReopen() last
	 * resolved for gameplay (GBA_VIDEO_HORIZONTAL/VERTICAL_PIXELS *
	 * s_scale) -- not necessarily an integer multiple of canvasWidth/
	 * canvasHeight, unlike VideoRTGBlit()'s own ScaleNearest() path, so
	 * ScaleNearestFit()'s per-axis ratio sampling is needed here instead.
	 * Reuses s_blitX/s_blitY, so a centered fullscreen picture centers
	 * the GUI canvas the same way. */
	unsigned scaledWidth = GBA_VIDEO_HORIZONTAL_PIXELS * s_scale;
	unsigned scaledHeight = GBA_VIDEO_VERTICAL_PIXELS * s_scale;

	size_t outPixels = (size_t) scaledWidth * scaledHeight;
	if (s_canvasScaledCapacity < (int) outPixels) {
		free(s_canvasScaledOutput);
		s_canvasScaledOutput = malloc(outPixels * sizeof(uint16_t));
		s_canvasScaledCapacity = s_canvasScaledOutput ? (int) outPixels : 0;
	}
	if (!s_canvasScaledOutput) {
		return;
	}

	ScaleNearestFit(s_canvasSwapped, canvasWidth, canvasWidth, canvasHeight,
		s_canvasScaledOutput, (int) scaledWidth, (int) scaledWidth, (int) scaledHeight);

	struct RenderInfo ri;
	ri.RGBFormat = RGBFB_B5G6R5PC;
	ri.Memory = s_canvasScaledOutput;
	ri.BytesPerRow = scaledWidth * sizeof(uint16_t);
	p96WritePixelArray(&ri, 0, 0, s_window->RPort, s_window->BorderLeft + s_blitX, s_window->BorderTop + s_blitY,
		scaledWidth, scaledHeight);
}

bool VideoRTGShouldQuit(void) {
	if (!s_window) {
		return true;
	}
	bool quit = false;
	struct IntuiMessage* msg;
	while ((msg = (struct IntuiMessage*) GetMsg(s_window->UserPort))) {
		switch (msg->Class) {
		case IDCMP_CLOSEWINDOW:
			quit = true;
			break;
		case IDCMP_RAWKEY: {
			/* High bit of Code marks a key release; the low 7 bits are
			 * the same raw key number either way (devices/inputevent.h). */
			bool keyUp = (msg->Code & IECODE_UP_PREFIX) != 0;
			UWORD rawKey = msg->Code & ~IECODE_UP_PREFIX;
			int key = _mapRawKey(rawKey);
			if (key != GBA_KEY_NONE) {
				if (keyUp) {
					s_keys &= ~(1U << key);
				} else {
					s_keys |= (1U << key);
				}
			}
			int guiKey = _mapRawKeyGui(rawKey);
			if (guiKey >= 0) {
				if (keyUp) {
					s_guiKeys &= ~(1U << guiKey);
				} else {
					s_guiKeys |= (1U << guiKey);
				}
			}
			break;
		}
		case IDCMP_MENUPICK: {
			enum MenuAction action = MenuDispatch(msg->Code);
			if (action != MENU_ACTION_NONE) {
				s_menuAction = action;
			}
			break;
		}
		}
		ReplyMsg((struct Message*) msg);
	}
	return quit;
}

uint32_t VideoRTGKeys(void) {
	return s_keys;
}

uint32_t VideoRTGGuiKeys(void) {
	return s_guiKeys;
}

enum MenuAction VideoRTGMenuAction(void) {
	enum MenuAction action = s_menuAction;
	s_menuAction = MENU_ACTION_NONE;
	return action;
}

struct Window* VideoRTGWindow(void) {
	return s_window;
}

bool VideoRTGFullscreen(void) {
	return s_fullscreen;
}

int VideoRTGScale(void) {
	return s_scale;
}

void VideoRTGDeinit(void) {
	MenuDeinit(s_window);
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
	free(s_scaledOutput);
	free(s_canvasSwapped);
	free(s_canvasScaledOutput);
	s_buffer = NULL;
	s_swapped = NULL;
	s_scaledOutput = NULL;
	s_canvasSwapped = NULL;
	s_canvasSwappedCapacity = 0;
	s_canvasScaledOutput = NULL;
	s_canvasScaledCapacity = 0;
}
