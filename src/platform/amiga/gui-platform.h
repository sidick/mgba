/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_GUI_PLATFORM_H
#define AMIGA_GUI_PLATFORM_H

#include "feature/gui/gui-runner.h"

/* Fills in every struct GUIParams field this platform provides
 * (drawStart/drawEnd/pollInput/pollCursor/batteryState/guiPrepare/
 * guiFinish/getText, width/height/font/basePath), runner->keySources,
 * and the rest of the mGUIRunner callback set (setup/teardown/drawFrame/
 * drawScreenshot/pollGameInput/running/incrementScreenMode;
 * gameLoaded/gameUnloaded/prepareForFrame/paused/unpaused/
 * setFrameLimiter are left NULL -- see this file's own .c comment on
 * why) -- everything mGUIInit()/gui-runner.c need from the platform
 * side to drive the shared ROM-browser/settings GUI and the in-game
 * loop on top of gui-font.c and video-rtg.c's existing pixel buffer/
 * blit pipeline.
 *
 * Must be called after VideoRTGInit() (binds GUIFontSetTarget() to its
 * buffer) and after mGUIInit() (binds default keys into the
 * already-initialized runner->params.keyMap; mGUIInit() itself calls
 * mInputMapInit() on it first). Does NOT call runner->core->
 * setVideoBuffer() itself -- that happens later, in the setup callback
 * registered here, once runner->core actually exists (mGUIRun() creates
 * it after this call, when a ROM is picked). */
void AmigaGuiPlatformInit(struct mGUIRunner* runner);

/* Frees what AmigaGuiPlatformInit() allocated (runner->params.font and
 * the GUI canvas) -- called from the registered teardown callback, not
 * meant to be called directly except by that callback itself. */
void AmigaGuiPlatformDeinit(struct mGUIRunner* runner);

#endif
