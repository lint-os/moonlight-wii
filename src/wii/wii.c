#include "wii.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <fat.h>

#include "gx_video.h"

void wii_fs_init(void) {
  fatInitDefault();
}

uint32_t currentFrame;
uint32_t nextFrame;

static frame_t frames[NUM_BUFFERS];
static pthread_mutex_t queueMutex;
static frame_t* queueMessages[MAX_QUEUEMESSAGES];
static uint32_t queueWriteIndex;
static uint32_t queueReadIndex;

void wii_stream_init(uint32_t width, uint32_t height) {
  gx_video_init();

  currentFrame = nextFrame = 0;
  pthread_mutex_init(&queueMutex, NULL);
  queueReadIndex = queueWriteIndex = 0;

  for (int i = 0; i < NUM_BUFFERS; i++) {
    frames[i].y = frames[i].u = frames[i].v = NULL;
  }
}

void wii_stream_draw(void) {
  frame_t* frame = get_frame();
  if (frame == NULL) {
    usleep(1000);
    return;
  }

  gx_video_draw_yuv(frame->y, frame->u, frame->v, frame->ystride, frame->uvstride);
}

void wii_stream_fini(void) {
  // the YUV frame buffers are freed by the decoder cleanup
}

void wii_setup_renderstate(void) {
  // GX state is configured on demand by gx_video_config_yuv()
}

void wii_present(void) {
  gx_video_present();
}

frame_t* get_frame(void) {
  pthread_mutex_lock(&queueMutex);

  uint32_t elements_in = queueWriteIndex - queueReadIndex;
  if (elements_in == 0) {
    pthread_mutex_unlock(&queueMutex);
    return NULL; // framequeue is empty
  }

  uint32_t i = (queueReadIndex)++ & (MAX_QUEUEMESSAGES - 1);
  frame_t* message = queueMessages[i];

  pthread_mutex_unlock(&queueMutex);
  return message;
}

void add_frame(frame_t* msg) {
  pthread_mutex_lock(&queueMutex);

  uint32_t elements_in = queueWriteIndex - queueReadIndex;
  if (elements_in == MAX_QUEUEMESSAGES) {
    pthread_mutex_unlock(&queueMutex);
    return; // framequeue is full
  }

  uint32_t i = (queueWriteIndex)++ & (MAX_QUEUEMESSAGES - 1);
  queueMessages[i] = msg;

  pthread_mutex_unlock(&queueMutex);
}

frame_t* wii_frame_buffer(int index) {
  return &frames[index];
}

void* wii_efb(void) {
  return NULL;
}

uint32_t wii_efb_width(void) {
  return (uint32_t) gx_video_efb_width();
}

uint32_t wii_efb_height(void) {
  return (uint32_t) gx_video_efb_height();
}

int wii_efb_bpp(void) {
  return 2;
}
