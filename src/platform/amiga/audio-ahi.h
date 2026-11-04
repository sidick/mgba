/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef AMIGA_AUDIO_AHI_H
#define AMIGA_AUDIO_AHI_H

#include <stdbool.h>

struct mCore;

/* Starts the dedicated audio process (see audio-ahi.c's own header
 * comment for why a second process, not just a callback run inline on
 * the main one) and opens ahi.device from inside it. Call once at
 * startup, before any core exists -- unlike VideoRTGBuffer(), there's
 * nothing here that needs a loaded ROM first.
 *
 * `unit` is the AHI device unit (0-3) to open -- each unit carries its
 * own independently-configured audio mode/driver preference via
 * SYS:Prefs/AHI, so this is how a caller targets a specific configured
 * output chain (e.g. one known to be set to Paula) rather than
 * whatever the system default happens to be. Pass a negative value
 * (e.g. -1) for "not specified", which resolves to AHI_DEFAULT_UNIT.
 *
 * Returns false (nothing audible, never fatal to the caller -- a
 * silent emulator still emulates) if ahi.device isn't available or the
 * audio process couldn't be started, the same "feature, not
 * requirement" convention GamepadInit()/VideoRTGInit() already use. */
bool AmigaAudioInit(int unit);

/* Wires this frontend's audio delivery (mAVStream's postAudioBuffer/
 * audioRateChanged hooks, core->setAudioBufferSize()) onto `core`.
 * Call once per core instance, any time after core->init() -- so once
 * at the very first ROM load, and again whenever _loadRom() (main.c)
 * builds a fresh core on a platform change, mirroring
 * _bindVideoBuffer()'s own per-core rebinding. A no-op (no crash, just
 * silence) if AmigaAudioInit() returned false or wasn't called. */
void AmigaAudioBindCore(struct mCore* core);

/* Mutes (or unmutes) output -- the ring buffer and the audio process's
 * own real-time pacing keep running exactly as before either way
 * (postAudioBuffer() still drains the core's own audio buffer every
 * call, so it doesn't fall behind and have to catch up on unmute);
 * only the samples handed to AHI go to silence. A no-op if
 * AmigaAudioInit() returned false or wasn't called. */
void AmigaAudioSetMuted(bool muted);

/* Stops the audio process and closes ahi.device. Safe to call even if
 * AmigaAudioInit() returned false or was never called. */
void AmigaAudioDeinit(void);

#endif
