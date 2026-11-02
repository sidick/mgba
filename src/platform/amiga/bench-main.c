/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Trimmed copy of src/platform/test/perf-main.c for the Amiga port: drops
 * the -D server mode and its unconditional mgba-util/socket.h dependency
 * (getaddrinfo/freeaddrinfo aren't available on classic AmigaOS TCP/IP
 * stacks -- Roadshow's headers predate the RFC 3493 API), keeping -F/-S/
 * -N/-P. Used to get a rough FPS/speed-multiplier reading before
 * committing to any GUI/video work. */
#include <mgba/core/cheats.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#ifdef M_CORE_GB
#include <mgba/gb/core.h>
#endif
#ifdef M_CORE_GBA
#include <mgba/gba/core.h>
#endif

#include <mgba/feature/commandline.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <inttypes.h>
#include <sys/time.h>

#define BENCH_OPTIONS "F:L:NPS:"
#define BENCH_USAGE \
	"Benchmark options:\n" \
	"  -F FRAMES        Run for the specified number of FRAMES before exiting\n" \
	"  -N               Disable video rendering entirely\n" \
	"  -P               CSV output, useful for parsing\n" \
	"  -S SEC           Run for SEC in-game seconds before exiting\n" \
	"  -L FILE          Load a savestate when starting the test"

struct BenchOpts {
	bool noVideo;
	bool csv;
	unsigned duration;
	unsigned frames;
	char* savestate;
};

static void _mBenchRunloop(struct mCore* context, int* frames, bool quiet);
static void _mBenchShutdown(int signal);
static bool _parseBenchOpts(struct mSubParser* parser, int option, const char* arg);
static void _log(struct mLogger*, int, enum mLogLevel, const char*, va_list);
static bool _mBenchRunCore(const char* fname, const struct mArguments*, const struct BenchOpts*);

static bool _dispatchExiting = false;
static struct VFile* _savestate = 0;
static void* _outputBuffer = NULL;

int main(int argc, char** argv) {
	signal(SIGINT, _mBenchShutdown);
	int didFail = 0;

	struct mLogger logger = { .log = _log };
	mLogSetDefaultLogger(&logger);

	struct BenchOpts benchOpts = { false, false, 0, 0, 0 };
	struct mSubParser subparser = {
		.usage = BENCH_USAGE,
		.parse = _parseBenchOpts,
		.extraOptions = BENCH_OPTIONS,
		.opts = &benchOpts
	};

	struct mArguments args = {};
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
	if (!args.fname) {
		parsed = false;
	}
	if (!parsed || args.showHelp) {
		usage(argv[0], NULL, NULL, &subparser, 1);
		didFail = !parsed;
		goto cleanup;
	}

	if (args.showVersion) {
		version(argv[0]);
		goto cleanup;
	}

	if (benchOpts.savestate) {
		_savestate = VFileOpen(benchOpts.savestate, O_RDONLY);
		free(benchOpts.savestate);
	}

	_outputBuffer = malloc(256 * 256 * 4);
	if (benchOpts.csv) {
		puts("game_code,frames,duration,renderer");
	}
	didFail = !_mBenchRunCore(args.fname, &args, &benchOpts);
	free(_outputBuffer);

	if (_savestate) {
		_savestate->close(_savestate);
	}
cleanup:
	mArgumentsDeinit(&args);

	return didFail;
}

bool _mBenchRunCore(const char* fname, const struct mArguments* args, const struct BenchOpts* benchOpts) {
	struct mCore* core = mCoreFind(fname);
	if (!core) {
		return false;
	}

	core->init(core);
	if (!benchOpts->noVideo) {
		core->setVideoBuffer(core, _outputBuffer, 256);
	}
	mCoreLoadFile(core, fname);
	mCoreConfigInit(&core->config, "bench");
	mCoreConfigLoad(&core->config);

	mCoreConfigSetOverrideIntValue(&core->config, "threadedVideo", 0);

	struct mCoreOptions opts = {};
	mCoreConfigMap(&core->config, &opts);
	opts.audioSync = false;
	opts.videoSync = false;
	mArgumentsApply(args, NULL, 0, &core->config);
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");
	mCoreLoadConfig(core);

	core->reset(core);
	if (_savestate) {
		mCoreLoadStateNamed(core, _savestate, 0);
	}

	struct mGameInfo info;
	core->getGameInfo(core, &info);

	int frames = benchOpts->frames;
	if (!frames) {
		frames = benchOpts->duration * 60;
	}
	struct timeval tv;
	gettimeofday(&tv, 0);
	uint64_t start = 1000000LL * tv.tv_sec + tv.tv_usec;
	_mBenchRunloop(core, &frames, benchOpts->csv);
	gettimeofday(&tv, 0);
	uint64_t end = 1000000LL * tv.tv_sec + tv.tv_usec;
	uint64_t duration = end - start;

	mCoreConfigFreeOpts(&opts);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);

	float scaledFrames = frames * 1000000.f;
	if (benchOpts->csv) {
		char buffer[256];
		const char* rendererName = benchOpts->noVideo ? "none" : "software";
		snprintf(buffer, sizeof(buffer), "%s-%s,%i,%" PRIu64 ",%s\n", info.system, info.code, frames, duration, rendererName);
		printf("%s", buffer);
	} else {
		printf("%u frames in %" PRIu64 " microseconds: %g fps (%gx)\n", frames, duration, scaledFrames / duration, scaledFrames / (duration * 60.f));
	}

	return true;
}

static void _mBenchRunloop(struct mCore* core, int* frames, bool quiet) {
	struct timeval lastEcho;
	gettimeofday(&lastEcho, 0);
	int duration = *frames;
	*frames = 0;
	int lastFrames = 0;
	while (!_dispatchExiting) {
		core->runFrame(core);
		++*frames;
		++lastFrames;
		if (!quiet) {
			struct timeval currentTime;
			long timeDiff;
			gettimeofday(&currentTime, 0);
			timeDiff = currentTime.tv_sec - lastEcho.tv_sec;
			timeDiff *= 1000;
			timeDiff += (currentTime.tv_usec - lastEcho.tv_usec) / 1000;
			if (timeDiff >= 1000) {
				printf("\033[2K\rCurrent FPS: %g (%gx)", lastFrames / (timeDiff / 1000.0f), lastFrames / (float) (60 * (timeDiff / 1000.0f)));
				fflush(stdout);
				lastEcho = currentTime;
				lastFrames = 0;
			}
		}
		if (duration > 0 && *frames == duration) {
			break;
		}
	}
	if (!quiet) {
		printf("\033[2K\r");
	}
}

static void _mBenchShutdown(int signal) {
	UNUSED(signal);
	_dispatchExiting = true;
}

static bool _parseBenchOpts(struct mSubParser* parser, int option, const char* arg) {
	struct BenchOpts* opts = parser->opts;
	errno = 0;
	switch (option) {
	case 'F':
		opts->frames = strtoul(arg, 0, 10);
		return !errno;
	case 'N':
		opts->noVideo = true;
		return true;
	case 'P':
		opts->csv = true;
		return true;
	case 'S':
		opts->duration = strtoul(arg, 0, 10);
		return !errno;
	case 'L':
		opts->savestate = strdup(arg);
		return true;
	default:
		return false;
	}
}

static void _log(struct mLogger* log, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(log);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}
