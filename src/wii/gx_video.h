#pragma once

#include <stdint.h>

// GX video pipeline (ported from WiiMC-SSLC: forwarder/video.c + mplayer/osdep/gx_supp.c).
//
// The GEKKO has no YUV texture formats, so video is: ffmpeg H264 -> YUV420P planes
// -> uploaded into GPU I8 textures -> TEV does the YUV->RGB conversion on the GPU
// (fast) -> rendered into the EFB -> GX_CopyDisp to the XFB -> VI out.
//
// Text (fonts) is rendered grayscale into the Y plane and drawn through the same
// YUV pipeline (like gx_supp.c's vo_draw_alpha_gekko).

// Mode-only init (early): pick the preferred mode and configure the VI.
void gx_video_init(void);
// XFB + GX init (late, after net/fs/input/font): allocate the XFBs, carve the
// MEM2 texture area, bring up the GPU, arm the flip callbacks.
void gx_video_init2(void);
void gx_video_fini(void);

// EFB dimensions (the mode's fbWidth/efbHeight).
int gx_video_efb_width(void);
int gx_video_efb_height(void);

// Configure the YUV->RGB TEV pipeline for a YUV420P frame of the given size.
// Call once per resolution (on stream start / when it changes).
void gx_video_config_yuv(int width, int height);

// Upload one decoded YUV420P frame and render+present it.
void gx_video_draw_yuv(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                       int ystride, int uvstride);

// Blit a full-screen grayscale (Y) image (the font buffer) through the YUV
// pipeline (neutral U/V) and present.
void gx_video_draw_gray(int width, int height, const uint8_t* gray, int gray_stride);

// Render whatever was queued to GX this frame and hand it to the VI.
void gx_video_present(void);
