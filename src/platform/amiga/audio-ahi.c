/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/* AHI (github.com/blombard/ahi -- the de facto standard retargetable
 * audio system most audio-capable Amiga software depends on, with its
 * own built-in Paula driver, so this one integration covers both
 * stock onboard sound and any real sound card the user has) via its
 * "Device Interface" (ahi.doc's own chapter of that name): a plain
 * CMD_WRITE stream of AHIST_S16S samples -- interleaved stereo signed
 * 16-bit, mgba's own native mAudioBuffer format exactly, no conversion
 * needed. Chosen over the "Function Interface" (AHI_AllocAudioA() +
 * sound IDs + SoundFunc/PlayerFunc hooks) because that API is built
 * for players/trackers juggling several simultaneous discrete sounds;
 * one continuous emulator audio stream is exactly the case the device
 * interface itself is documented for.
 *
 * _runDirect()'s own main loop (main.c) has no frame pacing of any
 * kind otherwise -- it runs core->runFrame() as fast as the host can
 * go. Audio can't tolerate that, and needs to be the thing that paces
 * playback to real time. mgba's own 3DS and PSP2 frontends establish
 * the pattern this follows: register an mAVStream whose
 * postAudioBuffer() hook the core calls inline, from inside
 * core->runFrame() itself, whenever it has produced a full buffer --
 * no polling needed. Where they differ is what actually throttles to
 * real time: 3DS has no audio thread at all and relies on its own
 * vsync to pace the main loop, with audio just riding along; PSP2 (no
 * vsync available to lean on, same as us) runs a dedicated audio
 * thread whose blocking hardware-output call IS the real-time clock,
 * and the core thread blocks on a condition variable once its own
 * lookahead buffers fill up. This file is that PSP2 shape, translated
 * to AmigaOS primitives: a second lightweight process (CreateNewProcTags,
 * the same idiom main.c's own stack-relaunch trick already uses) owns
 * ahi.device entirely and blocks on real AHI CMD_WRITE completions;
 * the main process's postAudioBuffer() callback just fills a small
 * shared ring buffer and blocks (Wait() on a signal, not a generic
 * condition variable -- the native AmigaOS equivalent) once that ring
 * is full. Both processes share one flat address space (no memory
 * protection on this OS), so the ring buffer itself is a plain static
 * struct -- no IPC message passing needed, just the ring's own
 * `full` flags and a pair of Exec signals for the two handoff
 * directions.
 *
 * Within the audio process, AHI's own documented double-buffering
 * recipe (ahidev.guide, "Writing To The Device") is layered on top of
 * the ring: two AHIRequests, each CMD_WRITE submitted via SendIO()
 * with its own ahir_Link pointing at the other, so the driver itself
 * can chain the next buffer the instant the current one finishes --
 * avoiding the audible gap a naive "submit, block, submit, block"
 * sequence would leave between buffers. */
#include "audio-ahi.h"

#include "debug-log.h"

#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba-util/audio-buffer.h>

#include <devices/ahi.h>
#include <dos/dostags.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <string.h>

/* One slot's worth of audio, in frames -- ~23ms at 44100Hz. Matches
 * core->setAudioBufferSize(), so postAudioBuffer() always has exactly
 * this many frames ready each time it's called (same convention the
 * 3DS frontend relies on). */
#define AMIGA_AUDIO_SAMPLES 1024

/* 3 slots: 2 stay pinned in flight at the AHI level at any moment
 * (the double-buffer pair below), leaving the producer exactly one
 * slot of lookahead room before postAudioBuffer() has to block --
 * enough to absorb normal scheduling jitter without needing a deeper
 * (higher-latency) ring. */
#define AMIGA_AUDIO_SLOTS 3

struct AmigaAudioSlot {
	int16_t samples[AMIGA_AUDIO_SAMPLES * 2];
	volatile bool full;
};

/* Shared by both processes -- see this file's own header comment for
 * why a plain static struct is enough on an OS with no memory
 * protection, and for the two-signal handoff design. */
static struct {
	struct AmigaAudioSlot slots[AMIGA_AUDIO_SLOTS];
	volatile unsigned writeIndex; /* owned by the main process */
	volatile unsigned readIndex;  /* owned by the audio process */
	volatile unsigned sampleRate;
	volatile int unit; /* AHI device unit to open -- set by AmigaAudioInit() before the process is spawned */
	volatile bool quit;
	volatile bool audioOk; /* set by the audio process before its ready signal */

	struct Task* mainTask;
	UBYTE mainReadySig; /* audio -> main: "started" (then later reused for "stopped") */
	UBYTE mainFreeSig;  /* audio -> main: "a slot was freed" */

	struct Task* audioTask;
	UBYTE audioDataSig; /* main -> audio: "a slot was filled" */
} s_shared;

static bool s_initialized;

/* Only ever touched by the main process (AmigaAudioSetMuted(), read by
 * _postAudioBuffer() below, both on that same process/task) -- no
 * cross-process signaling needed, unlike the slots' own `full` flags. */
static bool s_muted;

/* --- Audio process --- */

/* A real AHI/driver failure mid-session (card unplugged, driver crash)
 * would otherwise be entirely silent: WaitIO() still returns, and the
 * loop would just keep queuing and "completing" writes into the void
 * with nothing to notice. Logs the first failure only -- a wedged
 * driver returning the same error on every single write (one every
 * ~23ms) would otherwise flood the debug log without telling anyone
 * anything new after the first line. */
static void _logWriteErrorOnce(struct AHIRequest* req) {
	static bool logged;
	if (logged || !req->ahir_Std.io_Error) {
		return;
	}
	logged = true;
	AmigaDebugLogf("AHI CMD_WRITE failed, io_Error=%d (output may have silently stopped)\n",
		(int) req->ahir_Std.io_Error);
}

static void _audioProcessEntry(void) {
	s_shared.audioTask = FindTask(NULL);
	s_shared.audioDataSig = AllocSignal(-1);
	if (s_shared.audioDataSig == (UBYTE) -1) {
		s_shared.audioOk = false;
		Signal(s_shared.mainTask, 1UL << s_shared.mainReadySig);
		return;
	}

	struct MsgPort* port = CreateMsgPort();
	struct AHIRequest* reqA = port ? (struct AHIRequest*) CreateIORequest(port, sizeof(struct AHIRequest)) : NULL;
	struct AHIRequest* reqB = NULL;
	bool deviceOpen = false;
	if (reqA) {
		reqA->ahir_Version = 4;
		ULONG unit = s_shared.unit < 0 ? AHI_DEFAULT_UNIT : (ULONG) s_shared.unit;
		deviceOpen = !OpenDevice((CONST_STRPTR) AHINAME, unit, (struct IORequest*) reqA, 0);
	}
	if (deviceOpen) {
		/* ahidev.guide's own documented idiom for the second request of
		 * a double-buffered pair: a raw struct copy of the one that
		 * actually went through OpenDevice(), not a second
		 * CreateIORequest()+OpenDevice() -- io_Device/io_Unit and the
		 * reply port linkage all carry over correctly this way. */
		reqB = (struct AHIRequest*) AllocMem(sizeof(struct AHIRequest), MEMF_PUBLIC | MEMF_CLEAR);
		if (reqB) {
			CopyMem(reqA, reqB, sizeof(struct AHIRequest));
		} else {
			CloseDevice((struct IORequest*) reqA);
			deviceOpen = false;
		}
	}

	s_shared.audioOk = deviceOpen;
	Signal(s_shared.mainTask, 1UL << s_shared.mainReadySig);

	if (deviceOpen) {
		struct AHIRequest* reqs[2] = { reqA, reqB };
		/* Which ring slot each in-flight AHIRequest's buffer came from,
		 * so WaitIO()'s completion tells us exactly which slot to mark
		 * free again -- can't just free "the next readIndex", since by
		 * the time a request completes, readIndex has already moved on
		 * to the slot behind it (the other one still in flight). */
		unsigned reqSlot[2] = { 0, 0 };
		int cur = 0;
		bool havePrev = false;

		while (!s_shared.quit) {
			struct AmigaAudioSlot* slot = &s_shared.slots[s_shared.readIndex];
			while (!slot->full && !s_shared.quit) {
				Wait(1UL << s_shared.audioDataSig);
			}
			if (s_shared.quit) {
				break;
			}

			struct AHIRequest* req = reqs[cur];
			req->ahir_Std.io_Command = CMD_WRITE;
			req->ahir_Std.io_Data = slot->samples;
			req->ahir_Std.io_Length = AMIGA_AUDIO_SAMPLES * 2 * sizeof(int16_t);
			req->ahir_Std.io_Offset = 0;
			/* CMD_WRITE's own RESULT doc: every field except io_Device/
			 * io_Unit/io_Command is "trashed" by the previous completion
			 * -- io_Flags included, even though nothing here reads it
			 * back. Reset it explicitly rather than trust it's still 0
			 * from CreateIORequest()/the struct copy. */
			req->ahir_Std.io_Flags = 0;
			req->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
			req->ahir_Type = AHIST_S16S;
			req->ahir_Frequency = s_shared.sampleRate;
			req->ahir_Volume = 0x10000;
			req->ahir_Position = 0x8000;
			req->ahir_Link = havePrev ? reqs[1 - cur] : NULL;
			reqSlot[cur] = s_shared.readIndex;
			SendIO((struct IORequest*) req);

			s_shared.readIndex = (s_shared.readIndex + 1) % AMIGA_AUDIO_SLOTS;

			if (havePrev) {
				struct AHIRequest* prevReq = reqs[1 - cur];
				WaitIO((struct IORequest*) prevReq);
				_logWriteErrorOnce(prevReq);
				s_shared.slots[reqSlot[1 - cur]].full = false;
				Signal(s_shared.mainTask, 1UL << s_shared.mainFreeSig);
			}

			havePrev = true;
			cur = 1 - cur;
		}

		if (havePrev) {
			struct AHIRequest* prevReq = reqs[1 - cur];
			WaitIO((struct IORequest*) prevReq);
			_logWriteErrorOnce(prevReq);
			s_shared.slots[reqSlot[1 - cur]].full = false;
			Signal(s_shared.mainTask, 1UL << s_shared.mainFreeSig);
		}

		FreeMem(reqB, sizeof(struct AHIRequest));
		CloseDevice((struct IORequest*) reqA);
	}

	if (reqA) {
		DeleteIORequest((struct IORequest*) reqA);
	}
	if (port) {
		DeleteMsgPort(port);
	}

	/* Reused as the "I've stopped" acknowledgement -- AmigaAudioDeinit()
	 * waits on the same signal AmigaAudioInit() did for "I've started",
	 * since the two can never be pending at once. */
	Signal(s_shared.mainTask, 1UL << s_shared.mainReadySig);
}

/* --- Main process --- */

static void _postAudioBuffer(struct mAVStream* stream, struct mAudioBuffer* buffer) {
	UNUSED(stream);
	if (!s_initialized || !s_shared.audioOk) {
		return;
	}

	struct AmigaAudioSlot* slot = &s_shared.slots[s_shared.writeIndex];
	while (slot->full) {
		Wait(1UL << s_shared.mainFreeSig);
	}
	mAudioBufferRead(buffer, slot->samples, AMIGA_AUDIO_SAMPLES);
	if (s_muted) {
		memset(slot->samples, 0, sizeof(slot->samples));
	}
	slot->full = true;
	s_shared.writeIndex = (s_shared.writeIndex + 1) % AMIGA_AUDIO_SLOTS;
	Signal(s_shared.audioTask, 1UL << s_shared.audioDataSig);
}

static void _audioRateChanged(struct mAVStream* stream, unsigned sampleRate) {
	UNUSED(stream);
	if (sampleRate) {
		s_shared.sampleRate = sampleRate;
	}
}

static struct mAVStream s_stream = {
	.videoDimensionsChanged = NULL,
	.audioRateChanged = _audioRateChanged,
	.postVideoFrame = NULL,
	.postAudioFrame = NULL,
	.postAudioBuffer = _postAudioBuffer,
};

bool AmigaAudioInit(int unit) {
	memset(&s_shared, 0, sizeof(s_shared));
	s_shared.sampleRate = 44100;
	s_shared.unit = unit;
	s_shared.mainTask = FindTask(NULL);
	s_shared.mainReadySig = AllocSignal(-1);
	s_shared.mainFreeSig = AllocSignal(-1);
	if (s_shared.mainReadySig == (UBYTE) -1 || s_shared.mainFreeSig == (UBYTE) -1) {
		if (s_shared.mainReadySig != (UBYTE) -1) {
			FreeSignal(s_shared.mainReadySig);
		}
		if (s_shared.mainFreeSig != (UBYTE) -1) {
			FreeSignal(s_shared.mainFreeSig);
		}
		return false;
	}

	struct Process* child = CreateNewProcTags(
		NP_Entry, (ULONG) _audioProcessEntry,
		NP_StackSize, (ULONG) 16384,
		NP_Name, (ULONG) "mgba-amiga-audio",
		TAG_DONE);
	if (!child) {
		FreeSignal(s_shared.mainReadySig);
		FreeSignal(s_shared.mainFreeSig);
		return false;
	}

	Wait(1UL << s_shared.mainReadySig);
	s_initialized = true;
	if (!s_shared.audioOk) {
		/* The audio process already returned on its own (no ahi.device,
		 * or AllocSignal() failed inside it) -- nothing left to tear
		 * down there, just our own two signals. */
		AmigaDebugLogf("Could not open ahi.device, unit %d (running silent)\n", s_shared.unit);
		FreeSignal(s_shared.mainReadySig);
		FreeSignal(s_shared.mainFreeSig);
		s_initialized = false;
		return false;
	}
	return true;
}

void AmigaAudioBindCore(struct mCore* core) {
	if (!s_initialized || !s_shared.audioOk) {
		return;
	}
	core->setAudioBufferSize(core, AMIGA_AUDIO_SAMPLES);
	core->setAVStream(core, &s_stream);
}

void AmigaAudioSetMuted(bool muted) {
	s_muted = muted;
}

void AmigaAudioDeinit(void) {
	if (!s_initialized) {
		return;
	}
	s_shared.quit = true;
	Signal(s_shared.audioTask, 1UL << s_shared.audioDataSig);
	Wait(1UL << s_shared.mainReadySig);
	FreeSignal(s_shared.mainReadySig);
	FreeSignal(s_shared.mainFreeSig);
	s_initialized = false;
}
