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
 * to 256 (not the native 240) to match the convention other mgba frontends
 * (3DS, PSP2) already use, and because it's a clean multiple of 32 -- useful
 * if an AGA chunky-to-planar path is ever added later. */
#define AMIGA_VIDEO_STRIDE 256

/* Probes for Picasso96API.library and opens either a window on the current
 * public screen (fullscreen=false) or a dedicated RTG screen + borderless
 * window (fullscreen=true). Returns false if P96 isn't available or no
 * usable mode/screen/window could be opened -- the caller should fall back
 * to something else (currently: fail outright; AGA is a future option).
 *
 * `scale` is an integer pixel-replication factor (see scale.h) applied to
 * the whole GBA_VIDEO_HORIZONTAL_PIXELS x GBA_VIDEO_VERTICAL_PIXELS output
 * -- the window/screen is opened at exactly `scale` times that size. A
 * positive value is used exactly as given, with no further auto-fit.
 * Zero means "auto": in fullscreen mode, enumerate real display modes
 * (not a p96BestModeIDTags nominal-size request, which some drivers --
 * Z3660 observed -- will happily fabricate a CUSTOM mode for rather than
 * reporting what the board/monitor actually supports) and pick the
 * largest integer scale that fits the biggest one found; in windowed
 * mode zero just means the fixed default of 1, since there's no screen
 * of our own to measure. Passing 1 behaves exactly as before scaling
 * existed (no scaling pass, no extra buffer). */
bool VideoRTGInit(bool fullscreen, int scale);

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

/* Closes the window/screen and the library. Safe to call even if
 * VideoRTGInit() returned false (cleans up whatever got opened so far). */
void VideoRTGDeinit(void);

#endif
