/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_GAMEPAD_H
#define AMIGA_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>

/* Opens lowlevel.library (V40+, part of the base AmigaOS 3.1+ install --
 * confirmed present in a plain AmigaOS 3.2.2 install, no extra package
 * needed). Returns false if the library isn't available; this is never
 * fatal for the caller -- keyboard input (video-rtg.h) still works on its
 * own, a gamepad is a bonus. */
bool GamepadInit(void);

/* Polls the classic joystick port (physical "Port 2", traditionally the
 * joystick/gamepad port -- see gamepad.c's AMIGA_JOYPORT comment) right
 * now and returns a GBA_KEY_* (mgba/internal/gba/input.h) bitmask. Returns
 * 0 if GamepadInit() failed, or if nothing recognizable as a joystick/game
 * controller is currently plugged in.
 *
 * A real USB gamepad reports through this exact same lowlevel.library API
 * when Poseidon (github.com/AmigaPorts/poseidon) is installed -- it
 * patches ReadJoyPort() itself, so this code needs no Poseidon-specific
 * API calls to support one. */
uint32_t GamepadKeys(void);

/* Closes lowlevel.library. Safe to call even if GamepadInit() returned
 * false. */
void GamepadDeinit(void);

#endif
