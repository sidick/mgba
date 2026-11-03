/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "video-rtg.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/feature/commandline.h>

#include <stdio.h>

/* Input is not wired up yet (see video-rtg.h/.c): this is deliberately
 * video-plumbing-only for now, to validate the RTG blit path before
 * tackling joystick/keyboard-to-GBA-button mapping. The only way to quit
 * right now is the window's close gadget (windowed mode) or Ctrl-C from a
 * shell (fullscreen has no close gadget of its own yet). */
#define AMIGA_OPTIONS "F"
#define AMIGA_USAGE \
	"Amiga frontend options:\n" \
	"  -F               Open fullscreen (a dedicated RTG screen) instead of a window"

struct AmigaOpts {
	bool fullscreen;
};

static bool _parseAmigaOpts(struct mSubParser* parser, int option, const char* arg) {
	struct AmigaOpts* opts = parser->opts;
	UNUSED(arg);
	switch (option) {
	case 'F':
		opts->fullscreen = true;
		return true;
	default:
		return false;
	}
}

int main(int argc, char** argv) {
	struct AmigaOpts amigaOpts = { false };
	struct mSubParser subparser = {
		.usage = AMIGA_USAGE,
		.parse = _parseAmigaOpts,
		.extraOptions = AMIGA_OPTIONS,
		.opts = &amigaOpts
	};

	struct mArguments args = {};
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
	if (!args.fname) {
		parsed = false;
	}
	if (!parsed || args.showHelp) {
		usage(argv[0], NULL, NULL, &subparser, 1);
		mArgumentsDeinit(&args);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		mArgumentsDeinit(&args);
		return 0;
	}

	if (!VideoRTGInit(amigaOpts.fullscreen)) {
		fprintf(stderr, "Could not open an RTG screen/window (is Picasso96API.library installed?)\n");
		VideoRTGDeinit();
		mArgumentsDeinit(&args);
		return 1;
	}

	struct mCore* core = mCoreFind(args.fname);
	if (!core) {
		fprintf(stderr, "Could not find a core to run %s\n", args.fname);
		VideoRTGDeinit();
		mArgumentsDeinit(&args);
		return 1;
	}

	core->init(core);
	core->setVideoBuffer(core, VideoRTGBuffer(), AMIGA_VIDEO_STRIDE);
	mCoreLoadFile(core, args.fname);
	mCoreConfigInit(&core->config, "amiga");
	mCoreConfigLoad(&core->config);

	struct mCoreOptions opts = {};
	mCoreConfigMap(&core->config, &opts);
	mArgumentsApply(&args, NULL, 0, &core->config);
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);

	core->reset(core);

	bool quit = false;
	while (!quit) {
		core->runFrame(core);
		VideoRTGBlit();
		quit = VideoRTGShouldQuit();
	}

	mCoreConfigFreeOpts(&opts);
	mCoreConfigDeinit(&core->config);
	core->unloadROM(core);
	core->deinit(core);

	VideoRTGDeinit();
	mArgumentsDeinit(&args);
	return 0;
}
