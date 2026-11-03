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
 * guiFinish/getText, width/height/font/basePath) and runner->keySources
 * -- everything mGUIInit()/gui-runner.c need from the platform side to
 * drive the shared ROM-browser/settings GUI on top of gui-font.c and
 * video-rtg.c's existing pixel buffer/blit pipeline.
 *
 * Must be called after VideoRTGInit() (binds GUIFontSetTarget() to its
 * buffer) and after mGUIInit() (binds default keys into the
 * already-initialized runner->params.keyMap; mGUIInit() itself calls
 * mInputMapInit() on it first). */
void AmigaGuiPlatformInit(struct mGUIRunner* runner);

#endif
