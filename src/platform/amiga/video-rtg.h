/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_VIDEO_RTG_H
#define AMIGA_VIDEO_RTG_H

#include <stdbool.h>
#include <stdint.h>

/* Stride of the buffer handed to core->setVideoBuffer(), in pixels. Padded
 * to 256 (not the native 240) to match the convention other mgba frontends
 * (3DS, PSP2) already use, and because it's a clean multiple of 32 -- useful
 * if an AGA chunky-to-planar path is ever added later. */
#define AMIGA_VIDEO_STRIDE 256

/* Probes for Picasso96API.library and opens either a window on the current
 * public screen (fullscreen=false) or a dedicated RTG screen + borderless
 * window (fullscreen=true). Returns false if P96 isn't available or no
 * usable mode/screen/window could be opened -- the caller should fall back
 * to something else (currently: fail outright; AGA is a future option). */
bool VideoRTGInit(bool fullscreen);

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
