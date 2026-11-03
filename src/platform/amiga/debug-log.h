/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_DEBUG_LOG_H
#define AMIGA_DEBUG_LOG_H

#include <mgba/core/log.h>

/* Printf-style message to Copperline's WinUAE-compatible uaelib trap
 * (function 86 at $F0FF60): shows up host-side as "DBG: <text>" in the
 * terminal, control-protocol debug stream, and debugger console --
 * entirely independent of serial.device, unlike KPrintF()'s own
 * documented fallback. This matters here because a Shell '>SER:'
 * redirect and a wire client (e.g. AmiPilotServer SERIAL) both want
 * exclusive use of the one real/emulated serial port; routing our own
 * diagnostics through this trap instead means neither has to give way to
 * the other. Falls back to plain fprintf(stderr, ...) when the trap
 * isn't present (real hardware, or an emulator without it) -- same text
 * either way. */
ATTRIBUTE_FORMAT(printf, 1, 2)
void AmigaDebugLogf(const char* format, ...);

/* An mLogger that routes mgba's own mLog() calls through the same path
 * as AmigaDebugLogf() above, instead of mStandardLogger's stdout/file.
 * Use exactly like mStandardLoggerInit()/mLogSetDefaultLogger(): the
 * filter defaults to FATAL|ERROR, same as this frontend's previous
 * mStandardLogger setup. */
struct mLogger* AmigaDebugLoggerInit(void);
void AmigaDebugLoggerDeinit(struct mLogger* logger);

#endif
