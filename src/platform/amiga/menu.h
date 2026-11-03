/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_MENU_H
#define AMIGA_MENU_H

#include <stdbool.h>

#include <exec/types.h>
#include <intuition/intuition.h>

/* Standard Project-menu layout (Right-Amiga shortcuts, menu order, item
 * naming) follows the 1991 Amiga User Interface Style Guide ch. 6; Settings
 * uses a mutually-exclusive checked group for scale rather than a submenu
 * of radio buttons, since that's a menu's own idiom for "one of a small
 * fixed set" (radio gadgets are a window-gadget thing). */
enum MenuAction {
	MENU_ACTION_NONE = 0,
	MENU_ACTION_OPEN_ROM,
	MENU_ACTION_RESET,
	MENU_ACTION_PAUSE,
	MENU_ACTION_MUTE,
	MENU_ACTION_QUICK_SAVE,
	MENU_ACTION_QUICK_LOAD,
	MENU_ACTION_SCREENSHOT,
	MENU_ACTION_ABOUT,
	MENU_ACTION_QUIT,
	MENU_ACTION_SCALE_1X,
	MENU_ACTION_SCALE_2X,
	MENU_ACTION_SCALE_3X,
	MENU_ACTION_SCALE_AUTO,
	MENU_ACTION_TOGGLE_FULLSCREEN,
};

/* Opens gadtools.library, builds the menu strip, and attaches it to
 * `window` (which must already be open with IDCMP_MENUPICK requested and
 * WITHOUT WA_RMBTrap -- that flag steals the right mouse button from
 * Intuition's own menu-pulldown handling before it ever reaches the menu
 * system). `currentScale` (0 = auto, else 1/2/3) picks which Settings item
 * starts checked. Returns false (nothing attached, never fatal to the
 * caller -- a menu is a nicety) on any failure. */
bool MenuInit(struct Window* window, int currentScale);

/* Translates one IDCMP_MENUPICK message's Code into our own action enum,
 * following MenuItem->NextSelect for a multi-selected pick (Intuition
 * lets the user tick more than one item before releasing the menu
 * button) -- returns the first recognized action, since callers here only
 * care about one action per poll. MENU_ACTION_NONE if nothing recognized
 * (or no menu strip attached). */
enum MenuAction MenuDispatch(UWORD code);

/* Updates the Settings scale group's checkmark to match `scale` (0 = auto,
 * else 1/2/3) without rebuilding the whole strip -- ClearMenuStrip/mutate
 * MenuItem->Flags/ResetMenuStrip, the fast path intuition.doc documents
 * for CHECKED-only changes between calls. No-op if MenuInit() hasn't
 * succeeded. */
void MenuSetScale(struct Window* window, int scale);

/* Updates one independent (not part of a mutually-exclusive group like
 * scale) toggle item's checkmark to match `checked` -- the same
 * ClearMenuStrip/mutate/ResetMenuStrip fast path MenuSetScale() already
 * uses, just flipping one item's own CHECKED flag in isolation rather
 * than looping a group. `action` must name a CHECKIT item in s_menuDef
 * (currently MENU_ACTION_PAUSE or MENU_ACTION_MUTE). No-op if
 * MenuInit() hasn't succeeded. */
void MenuSetChecked(struct Window* window, enum MenuAction action, bool checked);

/* Opens the ASL "Open ROM" file requester (asl.library, opened on first
 * use). Returns a malloc'd path (caller frees) or NULL if cancelled or
 * asl.library isn't available. */
char* MenuOpenROM(struct Window* window);

/* A plain modal EasyRequest with mgba's own version string -- the
 * Style Guide's minimum bar for "About...". */
void MenuShowAbout(struct Window* window);

/* Detaches and frees the menu strip, VisualInfo, and closes gadtools.library
 * (and asl.library, if MenuOpenROM() ever opened it). Safe to call even if
 * MenuInit() was never called or failed; `window` may be NULL (skips
 * ClearMenuStrip, since there's nothing to detach from). */
void MenuDeinit(struct Window* window);

#endif
