/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_VIDEO_RTG_H
#define AMIGA_VIDEO_RTG_H

#include <stdbool.h>
#include <stdint.h>

#include "menu.h"

struct Window;

/* Stride of the buffer handed to core->setVideoBuffer(), in pixels. Padded
 * to 256 (not GBA's native 240) to match the convention other mgba frontends
 * (3DS, PSP2) already use, and because it's a clean multiple of 32 -- useful
 * if an AGA chunky-to-planar path is ever added later. Also happens to
 * exactly cover SGB_VIDEO_HORIZONTAL_PIXELS (256, mgba/internal/gb/video.h)
 * -- not a coincidence this frontend relies on, just convenient. */
#define AMIGA_VIDEO_STRIDE 256

/* Height of the VideoRTGInit()-allocated buffer -- SGB_VIDEO_VERTICAL_PIXELS
 * (224), the tallest content any mCore this frontend supports can report
 * from currentVideoSize() (GBA: 240x160; GB/GBC: 160x144; SGB with its
 * border enabled: 256x224). A smaller core's content simply uses less of
 * the buffer -- see VideoRTGSetContentSize(). */
#define AMIGA_VIDEO_MAX_HEIGHT 224

/* Probes for Picasso96API.library and opens either a window on the current
 * public screen (fullscreen=false) or a dedicated RTG screen + borderless
 * window (fullscreen=true). Returns false if P96 isn't available or no
 * usable mode/screen/window could be opened -- the caller should fall back
 * to something else (currently: fail outright; AGA is a future option).
 *
 * Content size defaults to GBA_VIDEO_HORIZONTAL/VERTICAL_PIXELS until a
 * caller tells VideoRTGSetContentSize() otherwise (e.g. once the actual
 * loaded core's real resolution is known) and calls VideoRTGReopen() to
 * apply it -- this lets the window open before any core exists to query,
 * the same way this function has always needed to.
 *
 * `scale` is an integer pixel-replication factor (see scale.h) applied to
 * the whole current content size -- the window/screen is opened at exactly
 * `scale` times that size. A positive value is used exactly as given, with
 * no further auto-fit. Zero means "auto": in fullscreen mode, enumerate
 * real display modes (not a p96BestModeIDTags nominal-size request, which
 * some drivers -- Z3660 observed -- will happily fabricate a CUSTOM mode
 * for rather than reporting what the board/monitor actually supports) and
 * pick the largest integer scale that fits the biggest one found; in
 * windowed mode zero just means the fixed default of 1, since there's no
 * screen of our own to measure. Passing 1 behaves exactly as before
 * scaling existed (no scaling pass, no extra buffer). */
bool VideoRTGInit(bool fullscreen, int scale);

/* Sets the content size used by VideoRTGReopen()'s own window/screen
 * sizing and VideoRTGBlit()'s own blit geometry, in place of the
 * GBA_VIDEO_HORIZONTAL/VERTICAL_PIXELS default -- intended for
 * core->currentVideoSize() (mgba/core/core.h), once a core is loaded, so
 * a GB/GBC ROM (160x144) gets a window sized for its own resolution
 * instead of GBA's (240x160). Does NOT itself reopen the window --
 * follow with VideoRTGReopen() to apply it (same fullscreen/scale as
 * before is fine; only the content size changed). Clamped to
 * AMIGA_VIDEO_STRIDE x AMIGA_VIDEO_MAX_HEIGHT, the buffer's own fixed
 * allocation. */
void VideoRTGSetContentSize(int width, int height);

/* Tears down and reopens the window/screen (and the menu strip, see menu.h)
 * at a new fullscreen/scale combination, same meaning as VideoRTGInit()'s own
 * parameters -- but keeps the GBA core's pixel buffer (VideoRTGBuffer()) and
 * Picasso96API.library handle alive, so the core's core->setVideoBuffer()
 * pointer stays valid across a live scale/mode change from the Settings
 * menu. VideoRTGInit() itself calls this once after its one-time setup, so
 * callers never need to call both. Returns false on failure, same as
 * VideoRTGInit(). */
bool VideoRTGReopen(bool fullscreen, int scale);

/* The window currently open (fullscreen or windowed) -- for passing to
 * MenuOpenROM()/MenuShowAbout(), which need a window to anchor their
 * requesters to. Never NULL between a successful VideoRTGInit()/
 * VideoRTGReopen() and VideoRTGDeinit(). */
struct Window* VideoRTGWindow(void);

/* The fullscreen/scale this window/screen is currently open at -- the
 * same values the most recent successful VideoRTGInit()/VideoRTGReopen()
 * call resolved to (scale is never 0 here, even if that call was passed
 * 0 for "auto": this reports the real, resolved scale). For a caller
 * (e.g. gui-platform.c's incrementScreenMode) that wants to cycle scale
 * without duplicating VideoRTGReopen()'s own fullscreen/auto-detect
 * logic. */
bool VideoRTGFullscreen(void);
int VideoRTGScale(void);

/* The menu action (see menu.h) picked since the last call, or
 * MENU_ACTION_NONE if none -- set by VideoRTGShouldQuit()'s IDCMP_MENUPICK
 * handling. Destructive read: calling this resets it to MENU_ACTION_NONE,
 * so call it exactly once per poll and store the result if it's needed more
 * than once. */
enum MenuAction VideoRTGMenuAction(void);

/* The buffer to pass to core->setVideoBuffer(core, buffer, AMIGA_VIDEO_STRIDE).
 * Owned by this module; valid between VideoRTGInit() and VideoRTGDeinit(). */
uint16_t* VideoRTGBuffer(void);

/* Converts the current buffer contents and blits them to the window/screen.
 * Call once per emulated frame, after core->runFrame(). */
void VideoRTGBlit(void);

/* Like VideoRTGBlit(), but for an arbitrary logical canvas instead of
 * VideoRTGBuffer() -- gui-platform.c's own GUI canvas, a fixed size
 * unrelated to GBA_VIDEO_HORIZONTAL/VERTICAL_PIXELS (see that file's own
 * comment on why the menu needs a canvas bigger than the GBA's native
 * resolution). `canvas` is `canvasWidth` x `canvasHeight` plain mColor
 * values, tightly packed (`canvasWidth` itself is the stride) -- scaled
 * with ScaleNearestFit() (scale.h) to fit whatever content size
 * VideoRTGReopen() last resolved, not necessarily an integer multiple of
 * the canvas the way VideoRTGBlit()'s own gameplay path always is. */
void VideoRTGBlitCanvas(const uint16_t* canvas, int canvasWidth, int canvasHeight);

/* Polls this window's IDCMP without blocking: tracks IDCMP_RAWKEY press/
 * release (see VideoRTGKeys() below) and returns true if the user closed
 * the window (close gadget / fullscreen screen-close equivalent), in which
 * case the frontend should exit its main loop. Call once per emulated
 * frame, before VideoRTGKeys(). */
bool VideoRTGShouldQuit(void);

/* GBA_KEY_* (mgba/internal/gba/input.h) bitmask of currently-held buttons,
 * built from this window's keyboard input by the most recent
 * VideoRTGShouldQuit() call. Fullscreen mode has no close gadget, so this
 * is currently the only way to drive the emulated machine -- there is no
 * joystick/gamepad support yet (see src/platform/amiga/main.c's -F usage
 * text for the key mapping). */
uint32_t VideoRTGKeys(void);

/* Bit positions VideoRTGGuiKeys() uses -- distinct from (and live at a
 * different time than) GBA_KEY_*, since mgba's shared gui-runner.c
 * ROM-browser/settings menu (see gui-platform.c) needs its own
 * Up/Down/Left/Right/Select/Back/Cancel navigation vocabulary, not the
 * in-game GBA button layout. */
enum AmigaGuiKey {
	AMIGA_GUI_KEY_UP = 0,
	AMIGA_GUI_KEY_DOWN,
	AMIGA_GUI_KEY_LEFT,
	AMIGA_GUI_KEY_RIGHT,
	AMIGA_GUI_KEY_SELECT,
	AMIGA_GUI_KEY_BACK,
	AMIGA_GUI_KEY_CANCEL,
};

/* AMIGA_GUI_KEY_* bitmask of currently-held menu-navigation keys, built
 * from the same keyboard input VideoRTGKeys() uses, by the most recent
 * VideoRTGShouldQuit() call. */
uint32_t VideoRTGGuiKeys(void);

/* Closes the window/screen and the library. Safe to call even if
 * VideoRTGInit() returned false (cleans up whatever got opened so far). */
void VideoRTGDeinit(void);

#endif
