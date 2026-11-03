/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_EMBEDDED_ASSETS_H
#define AMIGA_EMBEDDED_ASSETS_H

#include <stddef.h>

/* res/font-new.png (512x512, 8-bit grayscale-as-coverage) and
 * res/icons2x.png (512x128, 8-bit palette) baked in at build time by
 * bin2c.py (see CMakeLists.txt). font-new.png is the same asset Switch's
 * own gui-font.c uses -- see src/platform/amiga/gui-font.c's header
 * comment for the cell-grid math shared with it. */
extern const unsigned char g_amigaFontPng[];
extern const size_t g_amigaFontPng_size;

extern const unsigned char g_amigaIconsPng[];
extern const size_t g_amigaIconsPng_size;

#endif
