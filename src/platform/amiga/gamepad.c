/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gamepad.h"

#include <mgba/internal/gba/input.h>

#include <exec/types.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/lowlevel.h>

/* The lowlevel.library inline stubs (inline/lowlevel.h, pulled in via
 * proto/lowlevel.h) reference this exact global name -- it MUST be called
 * LowLevelBase. */
struct Library* LowLevelBase;

/* ReadJoyPort()'s port numbering is the hardware CIA numbering, not the
 * printed case labels: hardware port 0 ($DFF00A JOY0DAT) is the port
 * printed "1" on the case (conventionally mouse); hardware port 1
 * ($DFF00C JOY1DAT) is the port printed "2" (conventionally joystick/
 * gamepad) -- matches Copperline's own "port 2 = joystick" convention. */
#define AMIGA_JOYPORT 1

bool GamepadInit(void) {
	LowLevelBase = OpenLibrary("lowlevel.library", 40);
	return LowLevelBase != NULL;
}

uint32_t GamepadKeys(void) {
	if (!LowLevelBase) {
		return 0;
	}

	ULONG state = ReadJoyPort(AMIGA_JOYPORT);
	ULONG type = state & JP_TYPE_MASK;
	if (type != JP_TYPE_GAMECTLR && type != JP_TYPE_JOYSTK) {
		/* JP_TYPE_NOTAVAIL (nothing plugged in, or another task is mid-
		 * autosense), JP_TYPE_MOUSE, or JP_TYPE_UNKNOWN -- nothing to
		 * report as a gamepad. */
		return 0;
	}

	uint32_t keys = 0;
	if (state & JPF_JOY_UP) {
		keys |= 1U << GBA_KEY_UP;
	}
	if (state & JPF_JOY_DOWN) {
		keys |= 1U << GBA_KEY_DOWN;
	}
	if (state & JPF_JOY_LEFT) {
		keys |= 1U << GBA_KEY_LEFT;
	}
	if (state & JPF_JOY_RIGHT) {
		keys |= 1U << GBA_KEY_RIGHT;
	}

	if (type == JP_TYPE_GAMECTLR) {
		/* CD32-pad-style 7-button layout -- what a real CD32 pad reports
		 * natively, and what a USB gamepad reports too once Poseidon's
		 * HID class has patched ReadJoyPort() (see gamepad.h). Mapped
		 * for a 6-button GBA with one CD32 button (Reverse) left over:
		 * Red is the most common CD32 "primary fire" convention, so it
		 * gets A; Play is the near-universal CD32 Start substitute. */
		if (state & JPF_BUTTON_RED) {
			keys |= 1U << GBA_KEY_A;
		}
		if (state & JPF_BUTTON_BLUE) {
			keys |= 1U << GBA_KEY_B;
		}
		if (state & JPF_BUTTON_YELLOW) {
			keys |= 1U << GBA_KEY_L;
		}
		if (state & JPF_BUTTON_GREEN) {
			keys |= 1U << GBA_KEY_R;
		}
		if (state & JPF_BUTTON_FORWARD) {
			keys |= 1U << GBA_KEY_SELECT;
		}
		if (state & JPF_BUTTON_PLAY) {
			keys |= 1U << GBA_KEY_START;
		}
		/* JPF_BUTTON_REVERSE: no GBA button left to map it to. */
	} else {
		/* JP_TYPE_JOYSTK: a plain one/two-button joystick. Only Red
		 * (Fire) is guaranteed to exist; Blue (present on some
		 * two-button sticks) maps to B as a bonus when it's there. */
		if (state & JPF_BUTTON_RED) {
			keys |= 1U << GBA_KEY_A;
		}
		if (state & JPF_BUTTON_BLUE) {
			keys |= 1U << GBA_KEY_B;
		}
	}

	return keys;
}

void GamepadDeinit(void) {
	if (LowLevelBase) {
		CloseLibrary(LowLevelBase);
		LowLevelBase = NULL;
	}
}
