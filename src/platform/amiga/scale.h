/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_SCALE_H
#define AMIGA_SCALE_H

#include <stdint.h>

/* Nearest-neighbor (pixel-replication) integer upscale -- deliberately not
 * bilinear/HQ2x/any filtered scaler: this target's spare CPU budget is
 * unknown (see src/platform/amiga/main.c's interpreter-throughput
 * findings), so this stays the cheapest possible per-pixel operation
 * until there's a measured reason to spend more. Shared by the gameplay
 * blit path and, later, the GUI's own logical-canvas presentation --
 * see video-rtg.c's VideoRTGBlit() for the current consumer.
 *
 * `src` is `w` x `h` pixels, `srcStride` pixels per row. `dst` must be at
 * least `(w * scale)` x `(h * scale)` pixels, `dstStride` pixels per row
 * (dstStride must be >= w * scale). `scale` of 1 is a plain row-by-row
 * copy; callers on a hot path may prefer to skip calling this entirely at
 * scale 1 and blit `src` directly instead. */
void ScaleNearest(const uint16_t* src, int srcStride, int w, int h, uint16_t* dst, int dstStride, int scale);

/* Same nearest-neighbor policy as ScaleNearest() above, but for an
 * arbitrary (possibly non-integer, possibly downscaling) destination
 * size -- gui-platform.c's own logical canvas (fixed at 2x GBA
 * resolution) needs to fit whatever window/screen content size
 * VideoRTGReopen() resolved to, which won't always be an exact integer
 * multiple of that canvas. Per-axis ratio sampling (`srcX = dstX * srcW
 * / dstW`), the same technique gui-font.c's own icon scaling already
 * uses. `src` is `srcW` x `srcH`, `srcStride` pixels per row; `dst` is
 * `dstW` x `dstH`, `dstStride` pixels per row (dstStride must be >=
 * dstW). */
void ScaleNearestFit(const uint16_t* src, int srcStride, int srcW, int srcH, uint16_t* dst, int dstStride, int dstW, int dstH);

/* Largest integer scale of (w x h) that still fits within
 * (targetW x targetH), clamped to a minimum of 1 (never downscale --
 * a mode too small for even 1x is the caller's problem to reject
 * earlier, not this function's to silently shrink below native). */
int ScaleBestFit(int w, int h, int targetW, int targetH);

#endif
