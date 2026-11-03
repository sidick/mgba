/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "embedded-assets.h"
#include "gui-font-target.h"

#include <mgba-util/gui/font.h>
#include <mgba-util/gui/font-metrics.h>
#include <mgba-util/image/png-io.h>
#include <mgba-util/vfs.h>

#include <png.h>

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Software (CPU blit) counterpart to every other mgba port's GPU-texture
 * gui-font.c. Cell-grid layout and metric-doubling convention below are
 * checked against src/platform/switch/gui-font.c's GUIFontDrawGlyph --
 * the only other real consumer of font-new.png (res/font-new.png, 512x512
 * 8-bit grayscale-as-coverage): ASCII glyphs sit in a 16-column grid of
 * 32x32 cells (16 cols x 8 rows x 32px = 512x256 -- only the top half of
 * the square 512x512 atlas is actually used; Wii's font2x.png precedent
 * is the same 512x256 at native size, so font-new.png appears to just be
 * that same layout redrawn with antialiasing, padded square for whatever
 * reason upstream had). defaultFontMetrics (src/util/gui/font-metrics.c)
 * is authored in 1x units for the original 256x128 font.png -- every
 * consumer of a "2x" atlas (font2x.png there, font-new.png here) doubles
 * every width/height/padding value from it, so this does too.
 *
 * Icons (res/icons2x.png, 512x128 8-bit palette) use defaultIconMetrics'
 * x/y/width/height directly, also doubled -- same "2x asset, 1x-authored
 * metrics" convention. */
#define CELL_WIDTH 32
#define CELL_HEIGHT 32
#define GLYPH_HEIGHT 24

struct GUIFont {
	uint8_t* fontAtlas; /* 8-bit grayscale coverage, fontWidth x fontHeight */
	unsigned fontWidth;
	unsigned fontHeight;

	uint8_t* iconAtlas; /* RGBA8888, iconWidth x iconHeight -- see
	                      * _decodeRGBA()'s expand/add-alpha comment for
	                      * why this one needs decoding to 4 bytes/pixel
	                      * where the font gets away with 1. */
	unsigned iconWidth;
	unsigned iconHeight;
};

static uint8_t* _decodeGray(const unsigned char* data, size_t size, unsigned* outWidth, unsigned* outHeight) {
	struct VFile* vf = VFileFromConstMemory(data, size);
	if (!vf) {
		return NULL;
	}

	uint8_t* pixels = NULL;
	png_structp png = PNGReadOpen(vf, 0);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	png_infop end = png ? png_create_info_struct(png) : NULL;
	if (png && info && end && PNGReadHeader(png, info)) {
		unsigned width = png_get_image_width(png, info);
		unsigned height = png_get_image_height(png, info);
		pixels = malloc((size_t) width * height);
		if (pixels) {
			if (PNGReadPixels8(png, info, pixels, width, height, width) && PNGReadFooter(png, end)) {
				*outWidth = width;
				*outHeight = height;
			} else {
				free(pixels);
				pixels = NULL;
			}
		}
	}
	if (png) {
		PNGReadClose(png, info, end);
	}
	vf->close(vf);
	return pixels;
}

static uint8_t* _decodeRGBA(const unsigned char* data, size_t size, unsigned* outWidth, unsigned* outHeight) {
	struct VFile* vf = VFileFromConstMemory(data, size);
	if (!vf) {
		return NULL;
	}

	uint8_t* pixels = NULL;
	png_structp png = PNGReadOpen(vf, 0);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	png_infop end = png ? png_create_info_struct(png) : NULL;
	if (png && info && end && PNGReadHeader(png, info)) {
		/* icons2x.png is 8-bit palette; png_set_expand() handles
		 * palette-to-RGB, sub-8-bit-to-8-bit, and tRNS-to-alpha in one
		 * call; png_set_add_alpha() is a safety net in case the source
		 * has no tRNS chunk at all (then expand() alone yields 3
		 * channels, not 4).
		 *
		 * Deliberately NOT using mgba-util's own PNGReadPixelsA here:
		 * on a COLOR_16_BIT build (this one, src/platform/amiga/
		 * CMakeLists.txt's OS_DEFINES) it silently switches to writing
		 * packed RGB565 instead of RGBA8888 -- 2 bytes/pixel, alpha
		 * discarded, interpreting `stride` as a pixel count instead of a
		 * byte count. That's the right behavior for its real purpose
		 * (loading a screenshot straight into the core's own framebuffer
		 * format) but silently wrong for this one (we need the actual
		 * alpha channel as per-pixel coverage, and a plain RGBA8888
		 * buffer plays simpler with _drawIconRect's blending below) --
		 * found by hitting exactly this mismatch: PNGReadPixelsA
		 * returned false from a libpng error longjmp once the pixel-vs-
		 * byte stride confusion ran it past the allocated buffer. Read
		 * rows directly instead, matching that function's own non-
		 * COLOR_16_BIT code path unconditionally. */
		png_set_expand(png);
		png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
		png_read_update_info(png, info);

		unsigned width = png_get_image_width(png, info);
		unsigned height = png_get_image_height(png, info);
		pixels = malloc((size_t) width * height * 4);
		if (pixels && setjmp(png_jmpbuf(png)) == 0) {
			png_bytep row = malloc(png_get_rowbytes(png, info));
			if (row) {
				unsigned y;
				for (y = 0; y < height; ++y) {
					png_read_row(png, row, NULL);
					memcpy(&pixels[(size_t) y * width * 4], row, (size_t) width * 4);
				}
				free(row);
				if (PNGReadFooter(png, end)) {
					*outWidth = width;
					*outHeight = height;
				} else {
					free(pixels);
					pixels = NULL;
				}
			} else {
				free(pixels);
				pixels = NULL;
			}
		} else if (pixels) {
			/* Reached only via the setjmp error path -- a libpng error
			 * mid-read. */
			free(pixels);
			pixels = NULL;
		}
	}
	if (png) {
		PNGReadClose(png, info, end);
	}
	vf->close(vf);
	return pixels;
}

struct GUIFont* GUIFontCreate(void) {
	struct GUIFont* font = calloc(1, sizeof(*font));
	if (!font) {
		return NULL;
	}

	font->fontAtlas = _decodeGray(g_amigaFontPng, g_amigaFontPng_size, &font->fontWidth, &font->fontHeight);
	font->iconAtlas = _decodeRGBA(g_amigaIconsPng, g_amigaIconsPng_size, &font->iconWidth, &font->iconHeight);
	if (!font->fontAtlas || !font->iconAtlas) {
		GUIFontDestroy(font);
		return NULL;
	}
	return font;
}

void GUIFontDestroy(struct GUIFont* font) {
	if (!font) {
		return;
	}
	free(font->fontAtlas);
	free(font->iconAtlas);
	free(font);
}

unsigned GUIFontHeight(const struct GUIFont* font) {
	UNUSED(font);
	return GLYPH_HEIGHT;
}

unsigned GUIFontGlyphWidth(const struct GUIFont* font, uint32_t glyph) {
	UNUSED(font);
	if (glyph > 0x7F) {
		glyph = '?';
	}
	return defaultFontMetrics[glyph].width * 2;
}

void GUIFontIconMetrics(const struct GUIFont* font, enum GUIIcon icon, unsigned* w, unsigned* h) {
	UNUSED(font);
	if (icon >= GUI_ICON_MAX) {
		if (w) {
			*w = 0;
		}
		if (h) {
			*h = 0;
		}
		return;
	}
	if (w) {
		*w = defaultIconMetrics[icon].width * 2;
	}
	if (h) {
		*h = defaultIconMetrics[icon].height * 2;
	}
}

/* --- drawing: software blit into whatever GUIFontSetTarget() last gave us --- */

static uint16_t* s_target;
static int s_targetWidth;
static int s_targetHeight;
static int s_targetStride;

void GUIFontSetTarget(uint16_t* buffer, int width, int height, int stride) {
	s_target = buffer;
	s_targetWidth = width;
	s_targetHeight = height;
	s_targetStride = stride;
}

static inline uint16_t* _targetPixel(int x, int y) {
	if (!s_target || x < 0 || y < 0 || x >= s_targetWidth || y >= s_targetHeight) {
		return NULL;
	}
	return &s_target[y * s_targetStride + x];
}

/* mColor (COLOR_16_BIT + COLOR_5_6_5, see src/platform/amiga/
 * CMakeLists.txt): R in bits 0-4, G in bits 5-10, B in bits 11-15 -- same
 * layout video-rtg.c's own comment documents for the gameplay buffer.
 * This target buffer is plain logical mColor values, NOT the byte-swapped
 * P96-ready form video-rtg.c produces at blit time -- whatever eventually
 * blits a GUI canvas to screen is expected to byte-swap it the same way
 * VideoRTGBlit() already does for gameplay. */
static inline uint16_t _packColor(uint8_t r, uint8_t g, uint8_t b) {
	return (uint16_t) ((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
}

static inline void _unpackColor(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
	*r = (uint8_t) ((c & 0x1F) << 3);
	*g = (uint8_t) (((c >> 5) & 0x3F) << 2);
	*b = (uint8_t) (((c >> 11) & 0x1F) << 3);
}

static void _blendPixel(uint16_t* dst, unsigned alpha, uint8_t r, uint8_t g, uint8_t b) {
	if (!alpha) {
		return;
	}
	if (alpha >= 255) {
		*dst = _packColor(r, g, b);
		return;
	}
	uint8_t dr, dg, db;
	_unpackColor(*dst, &dr, &dg, &db);
	uint8_t outR = (uint8_t) ((r * alpha + dr * (255 - alpha)) / 255);
	uint8_t outG = (uint8_t) ((g * alpha + dg * (255 - alpha)) / 255);
	uint8_t outB = (uint8_t) ((b * alpha + db * (255 - alpha)) / 255);
	*dst = _packColor(outR, outG, outB);
}

/* Colors throughout this GUI API are 0xAABBGGRR (R in the lowest byte) --
 * checked against src/feature/gui/gui-runner.c's own literal color
 * constants (0xFFFFFFFF opaque white, 0x7FFFFFFF ~50%-alpha white) and
 * against Switch's gui-font.c, which unpacks the same way per-channel. */
#define COLOR_R(c) ((uint8_t) ((c) & 0xFF))
#define COLOR_G(c) ((uint8_t) (((c) >> 8) & 0xFF))
#define COLOR_B(c) ((uint8_t) (((c) >> 16) & 0xFF))
#define COLOR_A(c) ((uint8_t) (((c) >> 24) & 0xFF))

void GUIFontDrawGlyph(struct GUIFont* font, int x, int y, uint32_t color, uint32_t glyph) {
	if (glyph > 0x7F) {
		glyph = '?';
	}
	struct GUIFontGlyphMetric metric = defaultFontMetrics[glyph];

	uint8_t r = COLOR_R(color);
	uint8_t g = COLOR_G(color);
	uint8_t b = COLOR_B(color);
	uint8_t a = COLOR_A(color);

	int srcX = (glyph & 15) * CELL_WIDTH + metric.padding.left * 2;
	int srcY = (glyph >> 4) * CELL_HEIGHT + metric.padding.top * 2;
	int w = CELL_WIDTH - (metric.padding.left + metric.padding.right) * 2;
	int h = CELL_HEIGHT - (metric.padding.top + metric.padding.bottom) * 2;
	int dstX = x;
	int dstY = y - GLYPH_HEIGHT + metric.padding.top * 2;

	int row, col;
	for (row = 0; row < h; ++row) {
		int sy = srcY + row;
		if (sy < 0 || (unsigned) sy >= font->fontHeight) {
			continue;
		}
		const uint8_t* srcRow = &font->fontAtlas[(size_t) sy * font->fontWidth];
		for (col = 0; col < w; ++col) {
			int sx = srcX + col;
			if (sx < 0 || (unsigned) sx >= font->fontWidth) {
				continue;
			}
			unsigned alpha = (unsigned) srcRow[sx] * a / 255;
			uint16_t* dst = _targetPixel(dstX + col, dstY + row);
			if (dst) {
				_blendPixel(dst, alpha, r, g, b);
			}
		}
	}
}

/* Shared by GUIFontDrawIcon() (native size) and GUIFontDrawIconSize()
 * (arbitrary requested size): nearest-neighbor source sampling, same
 * "cheapest possible, no filtering" policy as scale.h's ScaleNearest --
 * when dstW/dstH equal srcW/srcH (the common DrawIcon case) this is a
 * plain 1:1 copy. */
static void _drawIconRect(const struct GUIFont* font, int dstX, int dstY, int dstW, int dstH,
	int srcX, int srcY, int srcW, int srcH, uint32_t color, bool hmirror, bool vmirror) {
	if (dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) {
		return;
	}

	uint8_t tr = COLOR_R(color);
	uint8_t tg = COLOR_G(color);
	uint8_t tb = COLOR_B(color);
	uint8_t ta = COLOR_A(color);

	int row, col;
	for (row = 0; row < dstH; ++row) {
		int sampleRow = row * srcH / dstH;
		int sy = srcY + (vmirror ? srcH - 1 - sampleRow : sampleRow);
		if (sy < 0 || (unsigned) sy >= font->iconHeight) {
			continue;
		}
		const uint8_t* srcRow = &font->iconAtlas[(size_t) sy * font->iconWidth * 4];
		for (col = 0; col < dstW; ++col) {
			int sampleCol = col * srcW / dstW;
			int sx = srcX + (hmirror ? srcW - 1 - sampleCol : sampleCol);
			if (sx < 0 || (unsigned) sx >= font->iconWidth) {
				continue;
			}
			const uint8_t* px = &srcRow[(size_t) sx * 4];
			unsigned alpha = (unsigned) px[3] * ta / 255;
			if (!alpha) {
				continue;
			}
			uint8_t r = (uint8_t) ((unsigned) px[0] * tr / 255);
			uint8_t g = (uint8_t) ((unsigned) px[1] * tg / 255);
			uint8_t b = (uint8_t) ((unsigned) px[2] * tb / 255);
			uint16_t* dst = _targetPixel(dstX + col, dstY + row);
			if (dst) {
				_blendPixel(dst, alpha, r, g, b);
			}
		}
	}
}

void GUIFontDrawIcon(struct GUIFont* font, int x, int y, enum GUIAlignment align, enum GUIOrientation orient, uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) {
		return;
	}
	struct GUIIconMetric metric = defaultIconMetrics[icon];
	int w = metric.width * 2;
	int h = metric.height * 2;

	switch (align & GUI_ALIGN_HCENTER) {
	case GUI_ALIGN_HCENTER:
		x -= w / 2;
		break;
	case GUI_ALIGN_RIGHT:
		x -= w;
		break;
	default:
		break;
	}
	switch (align & GUI_ALIGN_VCENTER) {
	case GUI_ALIGN_VCENTER:
		y -= h / 2;
		break;
	case GUI_ALIGN_BOTTOM:
		y -= h;
		break;
	default:
		break;
	}

	/* 90/270-degree rotations aren't implemented -- matches Wii's own
	 * gui-font.c precedent ("TODO: Rotations" there too) -- draw
	 * unrotated rather than silently drawing nothing. */
	bool hmirror = orient == GUI_ORIENT_HMIRROR;
	bool vmirror = orient == GUI_ORIENT_VMIRROR;
	_drawIconRect(font, x, y, w, h, metric.x * 2, metric.y * 2, w, h, color, hmirror, vmirror);
}

void GUIFontDrawIconSize(struct GUIFont* font, int x, int y, int w, int h, uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) {
		return;
	}
	struct GUIIconMetric metric = defaultIconMetrics[icon];
	if (!w) {
		w = metric.width * 2;
	}
	if (!h) {
		h = metric.height * 2;
	}
	_drawIconRect(font, x, y, w, h, metric.x * 2, metric.y * 2, metric.width * 2, metric.height * 2, color, false, false);
}
