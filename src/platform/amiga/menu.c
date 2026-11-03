/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "menu.h"

#include <mgba/core/version.h>

#include <exec/types.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>

/* The gadtools.library/asl.library inline stubs (inline/gadtools.h,
 * inline/asl.h, pulled in via proto/gadtools.h, proto/asl.h) reference
 * these exact global names -- same convention as video-rtg.c's P96Base
 * and gamepad.c's LowLevelBase. */
struct Library* GadToolsBase;
struct Library* AslBase;

static APTR s_visualInfo;
static struct Menu* s_menuStrip;

/* CreateMenusA() stashes nm_UserData per item/title in extra space right
 * after the struct Menu/MenuItem it builds; GTMENU_USERDATA()/
 * GTMENUITEM_USERDATA() (libraries/gadtools.h) are the only correct way
 * to read it back. */
#define USERDATA(action) ((APTR)(ULONG)(action))

/* Settings' scale group is a mutual-exclude set (CHECKIT, no MENUTOGGLE --
 * that flag is for independent toggle checkboxes, not this), each item's
 * nm_MutualExclude bit position matching its 0-based index *within this
 * menu title* (1x=0, 2x=1, 3x=2, Auto=3), not its index in this whole
 * array. */
static struct NewMenu s_menuDef[] = {
	{ NM_TITLE, (CONST_STRPTR) "Project",          NULL,      0,                 0,  NULL },
	{ NM_ITEM,  (CONST_STRPTR) "Open ROM...",      (CONST_STRPTR) "O", COMMSEQ,   0,  USERDATA(MENU_ACTION_OPEN_ROM) },
	{ NM_ITEM,  (CONST_STRPTR) NM_BARLABEL,        NULL,      0,                 0,  NULL },
	{ NM_ITEM,  (CONST_STRPTR) "Reset",            (CONST_STRPTR) "R", COMMSEQ,   0,  USERDATA(MENU_ACTION_RESET) },
	/* Pause is a plain independent toggle (CHECKIT, no nm_MutualExclude
	 * bits set) -- not part of the scale group below, which is a
	 * mutually-exclusive set. */
	{ NM_ITEM,  (CONST_STRPTR) "Pause",            (CONST_STRPTR) "P", COMMSEQ | CHECKIT, 0, USERDATA(MENU_ACTION_PAUSE) },
	{ NM_ITEM,  (CONST_STRPTR) NM_BARLABEL,        NULL,      0,                 0,  NULL },
	{ NM_ITEM,  (CONST_STRPTR) "Quick Save",       (CONST_STRPTR) "S", COMMSEQ,   0,  USERDATA(MENU_ACTION_QUICK_SAVE) },
	{ NM_ITEM,  (CONST_STRPTR) "Quick Load",       (CONST_STRPTR) "L", COMMSEQ,   0,  USERDATA(MENU_ACTION_QUICK_LOAD) },
	{ NM_ITEM,  (CONST_STRPTR) "Screenshot",       NULL,      0,                 0,  USERDATA(MENU_ACTION_SCREENSHOT) },
	{ NM_ITEM,  (CONST_STRPTR) NM_BARLABEL,        NULL,      0,                 0,  NULL },
	{ NM_ITEM,  (CONST_STRPTR) "About mGBA",       NULL,      0,                 0,  USERDATA(MENU_ACTION_ABOUT) },
	{ NM_ITEM,  (CONST_STRPTR) NM_BARLABEL,        NULL,      0,                 0,  NULL },
	{ NM_ITEM,  (CONST_STRPTR) "Quit mGBA",        (CONST_STRPTR) "Q", COMMSEQ,   0,  USERDATA(MENU_ACTION_QUIT) },
	{ NM_TITLE, (CONST_STRPTR) "Settings",         NULL,      0,                 0,  NULL },
	{ NM_ITEM,  (CONST_STRPTR) "1x",               NULL,      CHECKIT,           ~1, USERDATA(MENU_ACTION_SCALE_1X) },
	{ NM_ITEM,  (CONST_STRPTR) "2x",               NULL,      CHECKIT,           ~2, USERDATA(MENU_ACTION_SCALE_2X) },
	{ NM_ITEM,  (CONST_STRPTR) "3x",               NULL,      CHECKIT,           ~4, USERDATA(MENU_ACTION_SCALE_3X) },
	{ NM_ITEM,  (CONST_STRPTR) "Auto",             NULL,      CHECKIT | CHECKED, ~8, USERDATA(MENU_ACTION_SCALE_AUTO) },
	{ NM_ITEM,  (CONST_STRPTR) NM_BARLABEL,        NULL,      0,                 0,  NULL },
	{ NM_ITEM,  (CONST_STRPTR) "Toggle Fullscreen", NULL,     0,                 0,  USERDATA(MENU_ACTION_TOGGLE_FULLSCREEN) },
	{ NM_END,   NULL,                              NULL,      0,                 0,  NULL },
};

/* Index of "1x" in s_menuDef above -- MenuInit()'s own pre-selection
 * loop needs this; keep in sync with the array if its layout changes. */
#define SCALE_ITEM_FIRST 14

static int _scaleToIndex(int scale) {
	switch (scale) {
	case 1: return 0;
	case 2: return 1;
	case 3: return 2;
	default: return 3; /* 0 (auto), or anything else unexpected */
	}
}

bool MenuInit(struct Window* window, int currentScale) {
	GadToolsBase = OpenLibrary("gadtools.library", 36);
	if (!GadToolsBase) {
		return false;
	}

	/* Pre-select the matching scale item before the strip is ever built,
	 * rather than building with the array's own default (Auto) and
	 * immediately following up with a ClearMenuStrip/ResetMenuStrip. */
	int selected = _scaleToIndex(currentScale);
	int scaleItemFirst = SCALE_ITEM_FIRST;
	int i;
	for (i = 0; i < 4; ++i) {
		if (i == selected) {
			s_menuDef[scaleItemFirst + i].nm_Flags |= CHECKED;
		} else {
			s_menuDef[scaleItemFirst + i].nm_Flags &= ~CHECKED;
		}
	}

	s_visualInfo = GetVisualInfoA(window->WScreen, NULL);
	if (!s_visualInfo) {
		MenuDeinit(NULL);
		return false;
	}

	s_menuStrip = CreateMenusA(s_menuDef, NULL);
	if (!s_menuStrip || !LayoutMenusA(s_menuStrip, s_visualInfo, NULL)) {
		MenuDeinit(NULL);
		return false;
	}

	if (!SetMenuStrip(window, s_menuStrip)) {
		MenuDeinit(NULL);
		return false;
	}

	return true;
}

enum MenuAction MenuDispatch(UWORD code) {
	while (code != MENUNULL && s_menuStrip) {
		struct MenuItem* item = ItemAddress(s_menuStrip, code);
		if (!item) {
			break;
		}
		enum MenuAction action = (enum MenuAction) (ULONG) GTMENUITEM_USERDATA(item);
		if (action != MENU_ACTION_NONE) {
			return action;
		}
		code = item->NextSelect;
	}
	return MENU_ACTION_NONE;
}

/* Walks the strip gadtools built (struct Menu->NextMenu for titles,
 * MenuItem->NextItem for siblings under one title) rather than
 * reconstructing a MENUNUM/ITEMNUM code by hand -- matching by UserData
 * is the same information s_menuDef already encodes, without needing to
 * keep this in sync with the array's exact item ordering/indices. */
static struct MenuItem* _findItemByAction(enum MenuAction action) {
	struct Menu* menu;
	for (menu = s_menuStrip; menu; menu = menu->NextMenu) {
		struct MenuItem* item;
		for (item = menu->FirstItem; item; item = item->NextItem) {
			if ((enum MenuAction) (ULONG) GTMENUITEM_USERDATA(item) == action) {
				return item;
			}
		}
	}
	return NULL;
}

void MenuSetScale(struct Window* window, int scale) {
	if (!s_menuStrip) {
		return;
	}

	static const enum MenuAction kScaleActions[4] = {
		MENU_ACTION_SCALE_1X, MENU_ACTION_SCALE_2X, MENU_ACTION_SCALE_3X, MENU_ACTION_SCALE_AUTO
	};
	int selected = _scaleToIndex(scale);

	/* intuition.doc on ResetMenuStrip(): between a ClearMenuStrip() and
	 * the matching ResetMenuStrip(), CHECKED and ITEMENABLED are the
	 * *only* MenuItem fields allowed to change -- this is the sanctioned
	 * fast path for exactly this (toggling a checkmark), versus tearing
	 * down and rebuilding the whole strip. */
	ClearMenuStrip(window);
	int i;
	for (i = 0; i < 4; ++i) {
		struct MenuItem* item = _findItemByAction(kScaleActions[i]);
		if (!item) {
			continue;
		}
		if (i == selected) {
			item->Flags |= CHECKED;
		} else {
			item->Flags &= ~CHECKED;
		}
	}
	ResetMenuStrip(window, s_menuStrip);
}

void MenuSetPaused(struct Window* window, bool paused) {
	if (!s_menuStrip) {
		return;
	}

	struct MenuItem* item = _findItemByAction(MENU_ACTION_PAUSE);
	if (!item) {
		return;
	}

	ClearMenuStrip(window);
	if (paused) {
		item->Flags |= CHECKED;
	} else {
		item->Flags &= ~CHECKED;
	}
	ResetMenuStrip(window, s_menuStrip);
}

char* MenuOpenROM(struct Window* window) {
	if (!AslBase) {
		AslBase = OpenLibrary("asl.library", 36);
		if (!AslBase) {
			return NULL;
		}
	}

	struct FileRequester* fr = (struct FileRequester*) AllocAslRequestTags(ASL_FileRequest,
		ASLFR_Window, (ULONG) window,
		ASLFR_TitleText, (ULONG) "Open ROM",
		ASLFR_InitialPattern, (ULONG) "#?.gba",
		ASLFR_DoPatterns, TRUE,
		ASLFR_RejectIcons, TRUE,
		TAG_DONE);
	if (!fr) {
		return NULL;
	}

	char* path = NULL;
	if (AslRequest(fr, NULL)) {
		path = malloc(512);
		if (path) {
			path[0] = '\0';
			AddPart(path, fr->fr_Drawer, 512);
			AddPart(path, fr->fr_File, 512);
		}
	}
	FreeAslRequest(fr);
	return path;
}

void MenuShowAbout(struct Window* window) {
	struct EasyStruct es = {
		sizeof(struct EasyStruct),
		0,
		(CONST_STRPTR) "About mGBA",
		(CONST_STRPTR) "mGBA\nAmiga RTG frontend\nVersion %s",
		(CONST_STRPTR) "OK",
	};
	EasyRequest(window, &es, NULL, projectVersion);
}

void MenuDeinit(struct Window* window) {
	if (window && s_menuStrip) {
		ClearMenuStrip(window);
	}
	if (s_menuStrip) {
		FreeMenus(s_menuStrip);
		s_menuStrip = NULL;
	}
	if (s_visualInfo) {
		FreeVisualInfo(s_visualInfo);
		s_visualInfo = NULL;
	}
	if (GadToolsBase) {
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
	}
	if (AslBase) {
		CloseLibrary(AslBase);
		AslBase = NULL;
	}
}
