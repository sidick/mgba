/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "scale.h"

#include <string.h>

void ScaleNearest(const uint16_t* src, int srcStride, int w, int h, uint16_t* dst, int dstStride, int scale) {
	int y;
	for (y = 0; y < h; ++y) {
		const uint16_t* srcRow = &src[y * srcStride];
		uint16_t* dstRow = &dst[(y * scale) * dstStride];

		int x;
		for (x = 0; x < w; ++x) {
			uint16_t p = srcRow[x];
			int sx;
			for (sx = 0; sx < scale; ++sx) {
				dstRow[x * scale + sx] = p;
			}
		}

		/* The row above is now fully built -- replicate it downward
		 * rather than redoing the per-pixel work `scale` times. */
		int sy;
		for (sy = 1; sy < scale; ++sy) {
			memcpy(&dst[(y * scale + sy) * dstStride], dstRow, (size_t) w * scale * sizeof(uint16_t));
		}
	}
}

int ScaleBestFit(int w, int h, int targetW, int targetH) {
	int byWidth = targetW / w;
	int byHeight = targetH / h;
	int scale = byWidth < byHeight ? byWidth : byHeight;
	return scale < 1 ? 1 : scale;
}
