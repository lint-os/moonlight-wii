#include "wii.h"

#include <sps.h>

#include <libavcodec/avcodec.h>

#include <unistd.h>
#include <stdbool.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gx_video.h"

#define DECODER_BUFFER_SIZE (92 * 1024)

static AVCodec* codec;
static AVCodecContext* ctx;
static char* decodebuffer;

static int currentTexture;

static void wii_decoder_cleanup(void);

// Allocate the double-buffered YUV420P frame buffers for the stream resolution
// and configure the GPU YUV->RGB pipeline.
static int wii_alloc_frames(int width, int height) {
  for (int i = 0; i < NUM_BUFFERS; i++) {
    frame_t* fb = wii_frame_buffer(i);
    fb->width = width;
    fb->height = height;
    fb->ystride = (width + 7) & ~7;
    fb->uvstride = (width / 2 + 7) & ~7;

    free(fb->y);
    free(fb->u);
    free(fb->v);
    fb->y = memalign(64, (size_t) fb->ystride * height);
    fb->u = memalign(64, (size_t) fb->uvstride * (height / 2));
    fb->v = memalign(64, (size_t) fb->uvstride * (height / 2));
    if (fb->y == NULL || fb->u == NULL || fb->v == NULL) {
      fprintf(stderr, "Not enough memory for video buffers\n");
      free(fb->y);
      free(fb->u);
      free(fb->v);
      fb->y = fb->u = fb->v = NULL;
      return -1;
    }
  }
  return 0;
}

static int wii_decoder_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  if (videoFormat != VIDEO_FORMAT_H264) {
    printf("Invalid video format\n");
    return -1;
  }

  // ffmpeg 0.10 only exposes a codec to avcodec_find_decoder() after it has
  // been registered; the prebuilt WiiMC-SSLC lib does not auto-register.
  avcodec_register_all();

  codec = avcodec_find_decoder(CODEC_ID_H264);
  if (codec == NULL) {
    printf("No H264 decoder\n");
    return -1;
  }

  ctx = avcodec_alloc_context3(codec);
  if (ctx == NULL) {
    fprintf(stderr, "Not enough memory\n");
    return -1;
  }

  ctx->width = width;
  ctx->height = height;

  if (avcodec_open2(ctx, codec, NULL) < 0) {
    printf("Error opening H264 decoder\n");
    avcodec_close(ctx);
    av_free(ctx);
    ctx = NULL;
    return -1;
  }

  // GPU does the YUV->RGB conversion; no CPU swscale
  gx_video_config_yuv(width, height);
  if (wii_alloc_frames(width, height) != 0) {
    avcodec_close(ctx);
    av_free(ctx);
    ctx = NULL;
    return -1;
  }

  decodebuffer = memalign(64, DECODER_BUFFER_SIZE + 64);
  if (decodebuffer == NULL) {
    fprintf(stderr, "Not enough memory\n");
    wii_decoder_cleanup();
    return -1;
  }

  currentTexture = 0;

  return 0;
}

static void wii_decoder_cleanup() {
  if (ctx != NULL) {
    avcodec_close(ctx);
    av_free(ctx);
    ctx = NULL;
  }

  for (int i = 0; i < NUM_BUFFERS; i++) {
    frame_t* fb = wii_frame_buffer(i);
    free(fb->y);
    free(fb->u);
    free(fb->v);
    fb->y = fb->u = fb->v = NULL;
  }

  free(decodebuffer);
  decodebuffer = NULL;
}

static int wii_decoder_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  if (decodeUnit->fullLength > DECODER_BUFFER_SIZE) {
    fprintf(stderr, "Video decode buffer too small\n");
    return DR_OK;
  }

  int length = 0;
  PLENTRY entry = decodeUnit->bufferList;
  while (entry != NULL) {
    memcpy(decodebuffer + length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  }

  AVFrame frame;
  memset(&frame, 0, sizeof(frame));

  AVPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.data = decodebuffer;
  pkt.size = length;

  int got_frame = 0;
  if (avcodec_decode_video2(ctx, &frame, &got_frame, &pkt) < 0) {
    return DR_NEED_IDR;
  }

  if (!got_frame) {
    return DR_OK;
  }

  frame_t* fb = wii_frame_buffer(currentTexture);
  int w = frame.width < fb->width ? frame.width : fb->width;
  int h = frame.height < fb->height ? frame.height : fb->height;
  int hw = w / 2;
  int hh = h / 2;

  // copy the Y plane (padded to the 8-byte-aligned stride the GPU expects)
  for (int i = 0; i < h; i++) {
    memcpy(fb->y + (size_t) i * fb->ystride, frame.data[0] + (size_t) i * frame.linesize[0], w);
    memset(fb->y + (size_t) i * fb->ystride + w, 0, fb->ystride - w);
  }
  // copy the U and V planes
  for (int i = 0; i < hh; i++) {
    memcpy(fb->u + (size_t) i * fb->uvstride, frame.data[1] + (size_t) i * frame.linesize[1], hw);
    memset(fb->u + (size_t) i * fb->uvstride + hw, 0x80, fb->uvstride - hw);
    memcpy(fb->v + (size_t) i * fb->uvstride, frame.data[2] + (size_t) i * frame.linesize[2], hw);
    memset(fb->v + (size_t) i * fb->uvstride + hw, 0x80, fb->uvstride - hw);
  }

  nextFrame++;

  add_frame(fb);

  currentTexture++;
  if (currentTexture >= NUM_BUFFERS) {
    currentTexture = 0;
  }

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_wii = {
  .setup = wii_decoder_setup,
  .cleanup = wii_decoder_cleanup,
  .submitDecodeUnit = wii_decoder_submit_decode_unit,
  .capabilities = 0,
};
