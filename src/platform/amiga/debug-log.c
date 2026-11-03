/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "debug-log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The trap's call signature varies per function (see Copperline's
 * docs/guide/run.md); function 86 "Debug log string" takes just the
 * mode number and a NUL-terminated string, matching the vscode-amiga-
 * debug template's own KPrintF() (Copperline's guest/uaelib-test). */
typedef long (*AmigaUaeDebugLogFn)(long mode, const char* string);
#define AMIGA_UAELIB_TRAP ((volatile unsigned short*) 0xf0ff60)

/* WinUAE/Copperline map a JSR (0x4EB9) or, on AROS, an A-line (0xA00E)
 * at the trap address when it's actually implemented; anything else
 * means the address is unmapped (floating bus) -- the template's own
 * documented detection. */
static bool _uaelibPresent(void) {
	unsigned short word = *AMIGA_UAELIB_TRAP;
	return word == 0x4EB9 || word == 0xA00E;
}

static void _logRaw(const char* text) {
	if (_uaelibPresent()) {
		AmigaUaeDebugLogFn uaeDebugLog = (AmigaUaeDebugLogFn) (void*) 0xf0ff60;
		uaeDebugLog(86, text);
	} else {
		fputs(text, stderr);
	}
}

void AmigaDebugLogf(const char* format, ...) {
	char buffer[256];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	_logRaw(buffer);
}

/* Mirrors _mCoreStandardLog's own "%s: <message>\n" shape (src/core/log.c)
 * so FATAL/ERROR lines look the same here as they would on any other
 * mgba frontend, just delivered over the uaelib trap instead of stdout. */
static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	if (!mLogFilterTest(logger->filter, category, level)) {
		return;
	}

	char buffer[256];
	size_t length = snprintf(buffer, sizeof(buffer), "%s: ", mLogCategoryName(category));
	if (length < sizeof(buffer)) {
		length += vsnprintf(buffer + length, sizeof(buffer) - length, format, args);
	}
	if (length < sizeof(buffer) - 1) {
		buffer[length] = '\n';
		buffer[length + 1] = '\0';
	}
	_logRaw(buffer);
}

struct mLogger* AmigaDebugLoggerInit(void) {
	static struct mLogFilter filter;
	static struct mLogger logger;
	mLogFilterInit(&filter);
	filter.defaultLevels = mLOG_FATAL | mLOG_ERROR;
	logger.filter = &filter;
	logger.log = _log;
	return &logger;
}

void AmigaDebugLoggerDeinit(struct mLogger* logger) {
	mLogFilterDeinit(logger->filter);
}
