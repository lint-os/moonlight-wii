#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define VERSION_STRING "v1.0-wii"

enum {
  STATE_INVALID,
  STATE_DISCONNECTED,
  STATE_CONNECTING,
  STATE_CONNECTED,
  STATE_PAIRING,
  STATE_START_STREAM,
  STATE_STOP_STREAM,
  STATE_STREAMING,
};

#include <Limelight.h>
#include <stdint.h>
#include <stdbool.h>
#include "font.h"

extern int state;
extern int is_error;
extern char message_buffer[1024];

#define NUM_BUFFERS 2
#define MAX_QUEUEMESSAGES 16

typedef struct {
  uint8_t* y;
  uint8_t* u;
  uint8_t* v;
  uint32_t width;
  uint32_t height;
  uint32_t ystride;
  uint32_t uvstride;
} frame_t;

void wii_stream_init(uint32_t width, uint32_t height);
void wii_stream_draw(void);
void wii_stream_fini(void);
void wii_setup_renderstate(void);
void wii_present(void);

frame_t* get_frame(void);
void add_frame(frame_t* msg);
frame_t* wii_frame_buffer(int index);
void* wii_efb(void);
uint32_t wii_efb_width(void);
uint32_t wii_efb_height(void);
int wii_efb_bpp(void);

extern uint32_t nextFrame;

extern AUDIO_RENDERER_CALLBACKS audio_callbacks_wii;
extern DECODER_RENDERER_CALLBACKS decoder_callbacks_wii;

// input
void wii_input_init(void);
void wii_input_update(void); // this is only relevant while streaming
uint32_t wii_input_num_controllers(void);
uint32_t wii_input_buttons_triggered(void); // only really used for the menu
void start_input_thread(void);
void stop_input_thread(void);

// proc
void wii_proc_init(void);
void wii_proc_shutdown(void);
void wii_proc_register_home_callback(void);
int wii_proc_running(void);
void wii_proc_stop_running(void);
void wii_proc_set_home_enabled(int enabled);
void wii_proc_set_want_main_menu(int enabled);
int wii_proc_want_main_menu(void);

// net
void wii_net_init(void);
void wii_net_shutdown(void);

// startup TLS self-test (see tlstest.c)
void wii_tls_test(const char* target, const char* keydir);

// filesystem (SD card)
void wii_fs_init(void);
