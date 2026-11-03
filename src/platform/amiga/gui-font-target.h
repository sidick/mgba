/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_GUI_FONT_TARGET_H
#define AMIGA_GUI_FONT_TARGET_H

#include <stdint.h>

/* mgba's GUIFont API (mgba-util/gui/font.h) has no "destination buffer"
 * concept at all -- every existing platform draws straight into an
 * implicitly-bound GPU context (a texture quad, a GX display list, ...).
 * We have no GPU: GUIFontDrawGlyph()/DrawIcon()/DrawIconSize() need to
 * know which plain RGB565 pixel buffer to blit into. This is that
 * binding, an Amiga-specific addition on top of the portable API, not
 * part of it -- set it once before any Draw* call (the planned GUI
 * canvas's own init/frame-start code will be the real caller; nothing
 * wires this up yet). `buffer` must stay valid until the next call or
 * until drawing stops; passing NULL makes every Draw* call a no-op. */
void GUIFontSetTarget(uint16_t* buffer, int width, int height, int stride);

#endif
