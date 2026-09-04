/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "wii.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <opus/opus_multistream.h>

// Raw AVE DMA ring buffer (WiiMC-SSLC ao_gekko style), not the ASND voice
// API: one continuous 48 kHz stereo stream fed to the DSP via a ring of
// buffers. The DMA IRQ re-arms the next buffer; get_space() provides
// back-pressure. Decoded chunks are packed back-to-back through a carry
// buffer so no silence is inserted between packets (a chunk smaller than a
// buffer would otherwise leave a silence gap in that buffer).
#define BUFFER_SIZE  4096
#define BUFFER_COUNT 32
#define MAX_FRAMES   5760

static OpusMSDecoder* decoder;
static short* decodeBuf;
static short* stereoBuf;
static u8* carry;
static int carryFill;
static u8* ring;
static u8* silence;
static int channelCount;
static int sampleRate;
static int frameSize;
static int bufferFill;
static int bufferPlay;
static int buffered;
static int playing;
static int audioInited;

static int get_space(void) {
  return (BUFFER_SIZE * (BUFFER_COUNT - 2)) - buffered;
}

static void downmix(short* dst, const short* src, int frames, int channels) {
  if (channels == 2) {
    memcpy(dst, src, (size_t)frames * 2 * sizeof(short));
    return;
  }

  for (int i = 0; i < frames; i++) {
    const short* s = src + i * channels;
    s32 left = s[0];
    s32 right = s[1];
    s32 extra = 0;
    for (int c = 2; c < channels; c++) extra += s[c];
    s32 fold = extra / (channels - 2);
    left += fold;
    right += fold;
    dst[i * 2] = left > 32767 ? 32767 : (left < -32768 ? -32768 : (short)left);
    dst[i * 2 + 1] = right > 32767 ? 32767 : (right < -32768 ? -32768 : (short)right);
  }
}

// AVE DMA IRQ: queue the next ring buffer for the DSP.
static void dma_callback(void) {
  if (playing && buffered > 0) {
    AUDIO_InitDMA((u32)ring + bufferPlay * BUFFER_SIZE, BUFFER_SIZE);
    bufferPlay = (bufferPlay + 1) % BUFFER_COUNT;
    buffered -= BUFFER_SIZE;
  } else {
    AUDIO_InitDMA((u32)silence, BUFFER_SIZE);
  }
}

// Commit one full, fully-written carry buffer into the ring.
static void commit_carry(void) {
  u32 level;
  u8* dst = ring + bufferFill * BUFFER_SIZE;
  memcpy(dst, carry, BUFFER_SIZE);
  DCStoreRangeNoSync(dst, BUFFER_SIZE);
  level = IRQ_Disable();
  bufferFill = (bufferFill + 1) % BUFFER_COUNT;
  buffered += BUFFER_SIZE;
  IRQ_Restore(level);
  carryFill = 0;
}

// Append decoded stereo PCM to the ring, packing it back-to-back with the
// previous chunk (no silence gaps). Full buffers are written straight to the
// ring; the leftover partial is kept in the carry for the next chunk.
static void append_to_ring(const short* src, int total) {
  u32 level;

  if (carryFill > 0) {
    int space = BUFFER_SIZE - carryFill;
    int n = total < space ? total : space;
    memcpy(carry + carryFill, src, n);
    carryFill += n;
    src += n / (int)sizeof(short);
    total -= n;
    if (carryFill == BUFFER_SIZE)
      commit_carry();
  }

  while (total >= BUFFER_SIZE && get_space() >= BUFFER_SIZE) {
    u8* dst = ring + bufferFill * BUFFER_SIZE;
    memcpy(dst, src, BUFFER_SIZE);
    DCStoreRangeNoSync(dst, BUFFER_SIZE);
    level = IRQ_Disable();
    bufferFill = (bufferFill + 1) % BUFFER_COUNT;
    buffered += BUFFER_SIZE;
    IRQ_Restore(level);
    src += BUFFER_SIZE / (int)sizeof(short);
    total -= BUFFER_SIZE;
  }

  if (total > 0 && total < BUFFER_SIZE) {
    memcpy(carry, src, total);
    carryFill = total;
  }
}

static int ar_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  int rc;

  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams, opusConfig->coupledStreams, opusConfig->mapping, &rc);
  if (decoder == NULL) {
    printf("Failed to create opus decoder: %d\n", rc);
    return -1;
  }

  channelCount = opusConfig->channelCount;
  sampleRate = opusConfig->sampleRate;
  frameSize = opusConfig->samplesPerFrame;
  if (frameSize <= 0)
    frameSize = 960;

  decodeBuf = memalign(32, (size_t)MAX_FRAMES * channelCount * sizeof(short));
  stereoBuf = memalign(32, (size_t)MAX_FRAMES * 2 * sizeof(short));
  carry = memalign(32, BUFFER_SIZE);
  ring = memalign(32, (size_t)BUFFER_COUNT * BUFFER_SIZE);
  silence = memalign(32, BUFFER_SIZE);
  if (!decodeBuf || !stereoBuf || !carry || !ring || !silence) {
    printf("Audio buffer allocation failed\n");
    return -1;
  }

  for (int i = 0; i < BUFFER_COUNT; i++) {
    memset(ring + i * BUFFER_SIZE, 0, BUFFER_SIZE);
    DCFlushRange(ring + i * BUFFER_SIZE, BUFFER_SIZE);
  }
  memset(silence, 0, BUFFER_SIZE);
  DCFlushRange(silence, BUFFER_SIZE);

  if (!audioInited) {
    AUDIO_Init(NULL);
    audioInited = 1;
  }
  AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);

  bufferFill = bufferPlay = 0;
  buffered = 0;
  carryFill = 0;
  playing = 0;
  AUDIO_RegisterDMACallback(dma_callback);

  return 0;
}

static void ar_cleanup(void) {
  AUDIO_RegisterDMACallback(NULL);
  AUDIO_StopDMA();

  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }

  free(decodeBuf);
  decodeBuf = NULL;
  free(stereoBuf);
  stereoBuf = NULL;
  free(carry);
  carry = NULL;
  free(ring);
  ring = NULL;
  free(silence);
  silence = NULL;

  audioInited = 0;
}

static void ar_start(void) {
  bufferFill = bufferPlay = 0;
  buffered = 0;
  carryFill = 0;
  playing = 0;
  AUDIO_RegisterDMACallback(dma_callback);
}

static void ar_stop(void) {
  AUDIO_RegisterDMACallback(NULL);
  playing = 0;
  buffered = 0;
  carryFill = 0;
  AUDIO_StopDMA();
}

static void ar_decode_and_play_sample(char* data, int length) {
  if (decoder == NULL)
    return;

  // A NULL payload is a packet-loss placeholder: conceal exactly one frame
  // period, not MAX_FRAMES (which would dump a long silence blob).
  int maxFrames = (data == NULL) ? frameSize : MAX_FRAMES;
  int frames = opus_multistream_decode(decoder, data, length, decodeBuf, maxFrames, 0);
  if (frames <= 0)
    return;

  short* src = decodeBuf;
  if (channelCount > 2) {
    downmix(stereoBuf, decodeBuf, frames, channelCount);
    src = stereoBuf;
  }

  append_to_ring(src, frames * 2 * (int)sizeof(short));

  if (!playing && buffered > 0) {
    playing = 1;
    AUDIO_InitDMA((u32)ring + bufferPlay * BUFFER_SIZE, BUFFER_SIZE);
    AUDIO_StartDMA();
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_wii = {
  .init = ar_init,
  .start = ar_start,
  .stop = ar_stop,
  .cleanup = ar_cleanup,
  .decodeAndPlaySample = ar_decode_and_play_sample,
  .capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
