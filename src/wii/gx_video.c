// GX video pipeline for the Wii, ported from WiiMC-SSLC:
//   libs/forwarder/source/video.c  (InitVideo / StartGX / ResetVideo / Menu_Render)
//   source/mplayer/osdep/gx_supp.c (YUV->RGB conversion in TEV + YUV texture upload)
//
// The GEKKO has no YUV texture formats, so the H264 YUV420P planes are uploaded
// into I8 GPU textures and the TEV does the YUV->RGB conversion on the GPU (fast),
// then the EFB is copied to the XFB and handed to the VI. Fonts are blitted as a
// full-screen RGBA8 texture through the same pipeline.

#include "gx_video.h"
#include "mem2_manager.h"
#include "vi_encoder.h"

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#define FIFO_SIZE (384 * 1024)
#define ATTRIBUTE_ALIGN32 __attribute__((aligned(32)))

// Max Y/UV texture dimensions the oversized MEM2 buffers are sized for
// (WiiMC-SSLC source/video.h: MAX_WIDTH 1280, MAX_HEIGHT 1024).
#define MAX_WIDTH 1280
#define MAX_HEIGHT 1024

// video mode + double buffered target framebuffers (main memory, via SYS_AllocateFramebuffer)
static GXRModeObj* vmode;
static unsigned int* xfb[2];
static int xfb_alloc_w = 720, xfb_alloc_h = 576, xfb_bpp = 2;
static int whichfb = 0;
static int gx_presented = 0;
static int flip_pending = 0;
static int sync_interlace = 0;
static unsigned char* gp_fifo = NULL;
static u8* videoScreenshot = NULL;

// PAL/576i detection (WiiMC video.cpp:709-714): set in gx_video_init, used by
// HOffset (centering) and gx_video_init2 (xfbHeight).
static int pal = 0, want576i = 0, pal60 = 0;

// delayrender (WiiMC video.cpp:53 "fixes bottom screen garbage"): hold the VI
// black for the first few frames after init. The YUV path's first frames are
// garbage (uninitialized UV / mid-copy); WiiMC masks it by keeping the VI black
// until FrameTimer > 4. We count retraces in vblank_cb and un-black at frame 4.
static int delayrender_frames = 0;
#define DELAYRENDER_FRAMES 4

// YUV texture memory (main memory)
static u8* Yltexture = NULL;
static u8* Yrtexture = NULL;
static u8* Utexture = NULL;
static u8* Vtexture = NULL;
static u32 Yltexsize, Yrtexsize, UVtexsize;
static u16 Ylwidth, Yrwidth, Ywidth, Yheight, UVwidth, UVheight;
static GXTexObj YltexObj, YrtexObj, UtexObj, VtexObj;
static int st0, st1;
static int wl, wr;
static u16 Yrowpitch, UVrowpitch;

static f32 texcoordsY[16] ATTRIBUTE_ALIGN32 = {
  0, 0, 1, 0, 1, 1, 0, 1,
  0, 0, 1, 0, 1, 1, 0, 1
};
static f32 texcoordsUV[8] ATTRIBUTE_ALIGN32 = {
  0, 0, 1, 0, 1, 1, 0, 1
};

static s16 square[12] ATTRIBUTE_ALIGN32;
static GXColor colors[1] ATTRIBUTE_ALIGN32 = { { 0, 255, 0, 255 } };
static u8 dlist[32] ATTRIBUTE_ALIGN32;

// brightness/contrast (kept for the ported TEV stages; left at neutral)
static int levelconv = 1;
static float g_brightness = 0.0f;
static float g_contrast = 0.0f;

// YUV path camera (gx_supp.c): the video quad is drawn in a centered
// +/-haspect, +/-vaspect space viewed through this fixed camera.
static Mtx view;
typedef struct {
  guVector pos;
  guVector up;
  guVector view;
} camera;
static camera cam = {
  { 0.0f, 0.0f, 352.0f },
  { 0.0f, 0.5f, 0.0f },
  { 0.0f, 0.0f, -0.5f }
};

// ---------------------------------------------------------------------------
// Scaffolding (forwarder/video.c)
// ---------------------------------------------------------------------------

static void StartGX(void) {
  GXColor background = { 0, 0, 0, 0xff };
  gp_fifo = (unsigned char*) memalign(32, FIFO_SIZE);
  memset(gp_fifo, 0, FIFO_SIZE);
  GX_Init(gp_fifo, FIFO_SIZE);
  GX_SetCopyClear(background, 0x00ffffff);
  GX_SetDispCopyGamma(GX_GM_1_0);
  GX_SetCullMode(GX_CULL_NONE);
  GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  GX_SetAlphaUpdate(GX_TRUE);
}

// EFB->XFB copy configuration; depends only on the mode, set once.
static void setup_disp_copy(void) {
  f32 yscale = GX_GetYScaleFactor(vmode->efbHeight, vmode->xfbHeight);
  u32 xfbHeight = GX_SetDispCopyYScale(yscale);
  GX_SetScissor(0, 0, vmode->fbWidth, vmode->efbHeight);
  GX_SetDispCopySrc(0, 0, vmode->fbWidth, vmode->efbHeight);
  GX_SetDispCopyDst(vmode->fbWidth, xfbHeight);
  // WiiMC's InitVideo2 never calls GX_SetCopyFilter (uses the default copy
  // filter); it only applies deflicker/sharp later via SetDf/SetDfOff. Do not
  // set the mode's vfilter here -- the default is a non-zero deinterlace filter
  // and the YUV path inherits its "bottom screen garbage" (video.cpp:53).
  // WiiMC hard-disables field rendering (source/video.cpp): render whole frames.
  GX_SetFieldMode(GX_DISABLE,
                  ((vmode->viHeight == 2 * vmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
}

// Pre-retrace: hand the just-rendered buffer to the VI and toggle. Only flips
// when a frame is actually ready (flip_pending), matching the mplayer's
// vblank_cb. For interlaced modes we flip on one field per frame only.
//
// delayrender (WiiMC video.cpp:53): keep the VI black for the first
// DELAYRENDER_FRAMES retraces after init. The YUV path's first frames are
// garbage (uninitialized UV / mid-copy); un-blacking immediately exposes it as
// "bottom screen garbage". Count retraces here and un-black at frame 4.
static void vblank_cb(u32 retraceCnt) {
  if (delayrender_frames < DELAYRENDER_FRAMES) {
    delayrender_frames++;
    if (delayrender_frames == DELAYRENDER_FRAMES)
      VIDEO_SetBlack(FALSE);
    return;
  }

  int f = VIDEO_GetNextField();
  int doflip = (sync_interlace == 1) ? !f : (sync_interlace == 2) ? f : 1;
  if (!flip_pending || !doflip)
    return;
  VIDEO_SetNextFramebuffer(xfb[whichfb]);
  VIDEO_Flush();
  whichfb ^= 1;
  flip_pending = 0;
}

// GPU finished a frame: the buffer is ready to be shown at the next retrace.
static void draw_done_cb(void) {
  flip_pending = 1;
}

// Allocate the oversized Y/UV texture buffers in MEM2, exactly like WiiMC:
//   - reserve a MEM2_VIDEO area (wiimc.cpp:1169-1172), then
//   - GX_AllocTextureMemory (gx_supp.c:681) + the one-time memset (gx_supp.c:711).
// Allocated once (max size); every video/font frame reuses a subset of it.
static void gx_video_alloc_texture_memory(void) {
  u32 size = (1024 * MAX_HEIGHT)
           + ((MAX_WIDTH - 1024) * MAX_HEIGHT)
           + (1024 * (MAX_HEIGHT / 2) * 2)
           + (32 * 1024);

  if (!AddMem2Area(size, MEM2_VIDEO))
    return;

  Yltexture = (u8*) mem2_memalign(32, 1024 * MAX_HEIGHT, MEM2_VIDEO);
  Yrtexture = (u8*) mem2_memalign(32, (MAX_WIDTH - 1024) * MAX_HEIGHT, MEM2_VIDEO);
  Utexture  = (u8*) mem2_memalign(32, 1024 * (MAX_HEIGHT / 2), MEM2_VIDEO);
  Vtexture  = (u8*) mem2_memalign(32, 1024 * (MAX_HEIGHT / 2), MEM2_VIDEO);

  memset(Yltexture, 0, 1024 * MAX_HEIGHT);
  memset(Yrtexture, 0, (MAX_WIDTH - 1024) * MAX_HEIGHT);
  memset(Utexture, 0x80, 1024 * (MAX_HEIGHT / 2));
  memset(Vtexture, 0x80, 1024 * (MAX_HEIGHT / 2));
}

// PAL-aware VI centering + user display offset (WiiMC video.cpp:384-401).
static void HOffset(void) {
  if (pal) {
    vmode->viXOrigin = (VI_MAX_WIDTH_PAL - vmode->viWidth) / 2;
    vmode->viYOrigin = (VI_MAX_HEIGHT_PAL - vmode->viHeight) / 2;
  } else {
    vmode->viXOrigin = (VI_MAX_WIDTH_NTSC - vmode->viWidth) / 2;
    vmode->viYOrigin = (VI_MAX_HEIGHT_NTSC - vmode->viHeight) / 2;
  }

  s8 hoffset = 0;
  if (CONF_GetDisplayOffsetH(&hoffset) == 0)
    vmode->viXOrigin += hoffset;
}

// Mode-only init (WiiMC source/video.cpp InitVideo), called early: pick the
// preferred mode, detect PAL/576i/aspect, center it in the raster, and
// configure the VI. The XFBs and GX are set up later in gx_video_init2 (WiiMC
// InitVideo2), after net/fs/input and the font are up -- matching WiiMC's
// two-phase init order.
void gx_video_init(void) {
  VIDEO_Init();
  vmode = VIDEO_GetPreferredMode(NULL);
  vmode->viWidth = 704;

  // WiiMC video.cpp:709-714: detect PAL / 576i / PAL60 from the mode pointer
  // and the system video setting.
  if (vmode == &TVPal576IntDfScale || vmode == &TVPal576ProgScale) {
    pal = 1;
    if (vmode == &TVPal576IntDfScale)
      want576i = 1;
  } else if (CONF_GetVideo() == CONF_VIDEO_PAL)
    pal60 = 1;

  if (CONF_GetAspectRatio() == CONF_ASPECT_16_9)
    vmode->viWidth = 711;

  HOffset();

  VIDEO_SetBlack(TRUE);
  VIDEO_Configure(vmode);

  printf("[gx] vmode: fb=%d efb=%d xfb=%d vi=%dx%d origin=%d,%d pal=%d 576i=%d\n",
         vmode->fbWidth, vmode->efbHeight, vmode->xfbHeight,
         vmode->viWidth, vmode->viHeight, vmode->viXOrigin, vmode->viYOrigin,
         pal, want576i);
}

// XFB + GX init (WiiMC source/video.cpp InitVideo2), called late (after
// net/fs/input/font). Allocates the oversized XFBs, carves the MEM2 texture
// area, brings up the GPU, and arms the flip callbacks.
void gx_video_init2(void) {
  // Allocate the XFBs oversized (720x576) then display at 640x480, exactly like
  // WiiMC InitVideo2 -- avoids the green-line artifact on the first frame.
  vmode->fbWidth = 720;
  vmode->xfbHeight = 576;
  xfb_alloc_w = 720;
  xfb_alloc_h = 576;
  xfb_bpp = (vmode->viTVMode & VI_NON_INTERLACE) ? 4 : 2;
  xfb[0] = (u32*) SYS_AllocateFramebuffer(vmode);
  xfb[1] = (u32*) SYS_AllocateFramebuffer(vmode);
  DCInvalidateRange(xfb[0], VIDEO_GetFrameBufferSize(vmode));
  DCInvalidateRange(xfb[1], VIDEO_GetFrameBufferSize(vmode));
  xfb[0] = (u32*) MEM_K0_TO_K1(xfb[0]);
  xfb[1] = (u32*) MEM_K0_TO_K1(xfb[1]);

  VIDEO_ClearFrameBuffer(vmode, xfb[0], COLOR_BLACK);
  VIDEO_ClearFrameBuffer(vmode, xfb[1], COLOR_BLACK);

  // WiiMC video.cpp:747: display height follows the mode (576 for PAL 576i,
  // 480 otherwise). Do not force 480 on a 576-tall PAL mode.
  vmode->fbWidth = 640;
  vmode->xfbHeight = want576i ? 576 : 480;
  VIDEO_SetNextFramebuffer(xfb[0]);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  VIDEO_WaitVSync();

  VIDEO_SetTrapFilter(1);

  // Carve the MEM2 texture area after the XFBs are already allocated.
  gx_video_alloc_texture_memory();

  whichfb = 0;
  sync_interlace = (vmode->viTVMode & VI_NON_INTERLACE) ? 0 : 1;
  StartGX();
  setup_disp_copy();

  VIDEO_SetPreRetraceCallback(vblank_cb);
  GX_SetDrawDoneCallback(draw_done_cb);
  videoScreenshot = (u8*) mem2_malloc(vmode->fbWidth * vmode->efbHeight * 4, MEM2_VIDEO);
}

void gx_video_fini(void) {
  VIDEO_SetPreRetraceCallback(NULL);
  GX_SetDrawDoneCallback(NULL);
  GX_AbortFrame();
  GX_Flush();
  VIDEO_SetBlack(TRUE);
  VIDEO_Flush();

  if (Yltexture) mem2_free(Yltexture, MEM2_VIDEO);
  if (Yrtexture) mem2_free(Yrtexture, MEM2_VIDEO);
  if (Utexture)  mem2_free(Utexture, MEM2_VIDEO);
  if (Vtexture)  mem2_free(Vtexture, MEM2_VIDEO);
  Yltexture = Yrtexture = Utexture = Vtexture = NULL;
  RemoveMem2Area(MEM2_VIDEO);
}

int gx_video_efb_width(void) {
  return vmode ? vmode->fbWidth : 640;
}

int gx_video_efb_height(void) {
  return vmode ? vmode->efbHeight : 480;
}

// Submit the EFB->XFB copy to the GPU and let it run (non-blocking). The flip
// to the VI is NOT done here: it happens in vblank_cb at the next retrace,
// exactly like the mplayer (whose draw path never touches whichfb). Keeping the
// present non-blocking is what lets the menu and the stream keep pace with the
// retrace instead of stalling on it.
void gx_video_present(void) {
  GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_TRUE);
  GX_SetColorUpdate(GX_ENABLE);
  GX_CopyDisp(xfb[whichfb], GX_TRUE);
  GX_SetDrawDone();
  gx_presented = 1;
}

// ---------------------------------------------------------------------------
// Real-HW diagnostic: dump the uploaded Y/U/V textures and the final XFB to SD
// as BMPs. Bisects the "bottom screen garbage": if the Y texture is a correct
// frame but the XFB is garbled, the bug is in the TEV/draw/present; if the Y
// texture is already garbled, it is the DC/MEM2 upload.
// ---------------------------------------------------------------------------

static void bmp24_header(FILE* f, int w, int h) {
  u32 rowpad = (4 - ((w * 3) & 3)) & 3;
  u32 imgsize = (u32) (w * 3 + rowpad) * h;
  u32 filesize = 54 + imgsize;
  u16 zero16 = 0, planes = 1, bpp = 24;
  u32 offset = 54, dibsize = 40, comp = 0, xppm = 0, yppm = 0, cu = 0, ci = 0;
  fwrite("BM", 1, 2, f);
  fwrite(&filesize, 4, 1, f);
  fwrite(&zero16, 2, 1, f);
  fwrite(&zero16, 2, 1, f);
  fwrite(&offset, 4, 1, f);
  fwrite(&dibsize, 4, 1, f);
  fwrite(&w, 4, 1, f);
  fwrite(&h, 4, 1, f);
  fwrite(&planes, 2, 1, f);
  fwrite(&bpp, 2, 1, f);
  fwrite(&comp, 4, 1, f);
  fwrite(&imgsize, 4, 1, f);
  fwrite(&xppm, 4, 1, f);
  fwrite(&yppm, 4, 1, f);
  fwrite(&cu, 4, 1, f);
  fwrite(&ci, 4, 1, f);
}

static void dump_gray_bmp(const char* path, const u8* data, int w, int h, int stride) {
  FILE* f = fopen(path, "wb");
  if (!f)
    return;
  bmp24_header(f, w, h);
  u8* row = (u8*) memalign(4, w * 3 + 4);
  u8 pad[4] = { 0, 0, 0, 0 };
  u32 rowpad = (4 - ((w * 3) & 3)) & 3;
  for (int y = h - 1; y >= 0; y--) {
    const u8* src = data + (size_t) y * stride;
    for (int x = 0; x < w; x++) {
      u8 v = src[x];
      row[x * 3] = v;
      row[x * 3 + 1] = v;
      row[x * 3 + 2] = v;
    }
    fwrite(row, 1, w * 3, f);
    if (rowpad)
      fwrite(pad, 1, rowpad, f);
  }
  free(row);
  fclose(f);
}

static void dump_xfb_bmp(const char* path, const void* data, int w, int stride, int h, int bpp) {
  FILE* f = fopen(path, "wb");
  if (!f)
    return;
  bmp24_header(f, w, h);
  u8* row = (u8*) memalign(4, w * 3 + 4);
  u8 pad[4] = { 0, 0, 0, 0 };
  u32 rowpad = (4 - ((w * 3) & 3)) & 3;
  for (int y = h - 1; y >= 0; y--) {
    const u8* src = (const u8*) data + (size_t) y * stride * bpp;
    for (int x = 0; x < w; x++) {
      if (bpp == 2) {
        u16 p = ((const u16*) src)[x];
        u8 r5 = (p >> 11) & 0x1f, g6 = (p >> 5) & 0x3f, b5 = p & 0x1f;
        row[x * 3] = (r5 << 3) | (r5 >> 2);
        row[x * 3 + 1] = (g6 << 2) | (g6 >> 4);
        row[x * 3 + 2] = (b5 << 3) | (b5 >> 2);
      } else {
        row[x * 3] = src[x * 4];
        row[x * 3 + 1] = src[x * 4 + 1];
        row[x * 3 + 2] = src[x * 4 + 2];
      }
    }
    fwrite(row, 1, w * 3, f);
    if (rowpad)
      fwrite(pad, 1, rowpad, f);
  }
  free(row);
  fclose(f);
}

static void gx_video_dump_debug(u32 frame) {
  char path[64];

  // Force a MEM2 read (bypass the CPU data cache) so we dump what the GPU
  // actually sees, not the (always-correct) cache we just wrote. If the
  // DCFlushRange of the upload failed to reach MEM2, the Y/U/V below turn to
  // garbage while the on-screen XFB is also garbage; if they stay clean, the
  // upload is fine and the bug is in the GPU draw/present.
  if (Yltexture) {
    DCInvalidateRange(Yltexture, Yltexsize);
    snprintf(path, sizeof(path), "/moonlight/dbg_y_%03u.bmp", frame);
    dump_gray_bmp(path, Yltexture, Ylwidth, Yheight, Ylwidth);
  }
  if (wr > 0 && Yrtexture) {
    DCInvalidateRange(Yrtexture, Yrtexsize);
    snprintf(path, sizeof(path), "/moonlight/dbg_yr_%03u.bmp", frame);
    dump_gray_bmp(path, Yrtexture, Yrwidth, Yheight, Yrwidth);
  }
  if (Utexture) {
    DCInvalidateRange(Utexture, UVtexsize);
    DCInvalidateRange(Vtexture, UVtexsize);
    snprintf(path, sizeof(path), "/moonlight/dbg_u_%03u.bmp", frame);
    dump_gray_bmp(path, Utexture, UVwidth, UVheight, UVwidth);
    snprintf(path, sizeof(path), "/moonlight/dbg_v_%03u.bmp", frame);
    dump_gray_bmp(path, Vtexture, UVwidth, UVheight, UVwidth);
  }
  // Dump the EFB directly -- the one buffer we've never captured. The EFB is at
  // K1 0x80000000, 640px wide (fixed stride), now RGB8 (4B/px). If this is a
  // clean frame but the XFB below is garbled, the bug is in the EFB->XFB copy;
  // if this is garbled too, the bug is in the draw.
  {
    const void* efb = (const void*) 0x80000000;
    GX_WaitDrawDone();
    snprintf(path, sizeof(path), "/moonlight/dbg_efb_%03u.bmp", frame);
    dump_xfb_bmp(path, efb, vmode->fbWidth, 640, vmode->efbHeight, 4);
  }
  if (xfb[whichfb]) {
    DCInvalidateRange(xfb[whichfb], VIDEO_GetFrameBufferSize(vmode));
    snprintf(path, sizeof(path), "/moonlight/dbg_xfb_%03u.bmp", frame);
    dump_xfb_bmp(path, xfb[whichfb], vmode->fbWidth, xfb_alloc_w,
                 vmode->efbHeight, xfb_bpp);
  }
  printf("[dbg] dumped frame %u (Y %dx%d wr=%d UV %dx%d)\n",
         frame, Ylwidth, Yheight, wr, UVwidth, UVheight);
}

static u32 dbg_frame = 0;
static const u32 dbg_at[] = { 16, 64, 256 };

static void gx_video_dbg_maybe(void) {
  dbg_frame++;
  for (unsigned i = 0; i < sizeof(dbg_at) / sizeof(dbg_at[0]); i++) {
    if (dbg_frame == dbg_at[i]) {
      gx_video_dump_debug(dbg_frame);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// YUV path: TEV YUV->RGB (gx_supp.c)
// ---------------------------------------------------------------------------

static void draw_initYUV(void) {
  // YUV->RGB formulation 3 (BT.709 coefficients; brightness/contrast neutral)
  static const GXColor uv_coeffs[2] = {
    { 200, 59, 0, 255 },   // {1.5701/2, 0.4664/2, 0}
    { 0, 11, 118, 255 }    // {0, 0.187/4, 1.8556/4}
  };

  GX_SetNumChans(1);
  GX_SetNumTexGens(4);
  GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
  GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
  GX_SetTexCoordGen(GX_TEXCOORD2, GX_TG_MTX2x4, GX_TG_TEX2, GX_IDENTITY);

  GX_SetNumTevStages(15);
  GX_SetTevKColor(GX_KCOLOR0, (GXColor){ 255, 0, 0, levelconv ? 18 : 0 });
  GX_SetTevKColor(GX_KCOLOR1, (GXColor){ 0, 0, 255, levelconv ? 41 : 0 });

  int brightness_offset;
  u8 contrast_konst, brightness_konst;
  int contrast_InD;
  if (g_contrast <= 0.0f) {
    contrast_konst = (u8) ((1 + (g_contrast * 0.5)) * 255);
    brightness_offset = -((int) contrast_konst) / 2 + 128 + ((int) (g_brightness * 255));
    contrast_InD = GX_CC_ZERO;
  } else {
    contrast_konst = (u8) (g_contrast * 255);
    brightness_offset = -((int) contrast_konst) / 2 + ((int) (g_brightness * 255));
    contrast_InD = GX_CC_CPREV;
  }
  if (brightness_offset > 255) {
    brightness_konst = (u8) (brightness_offset - 128);
  } else if (brightness_offset >= 0) {
    brightness_konst = (u8) (brightness_offset);
  } else if (brightness_offset >= -255) {
    brightness_konst = (u8) (-brightness_offset);
  } else {
    brightness_konst = (u8) (-brightness_offset - 128);
  }

  GXColor KColor2 = (GXColor){ uv_coeffs[0].r, uv_coeffs[0].g, uv_coeffs[0].b, contrast_konst };
  GXColor KColor3 = (GXColor){ uv_coeffs[1].r, uv_coeffs[1].g, uv_coeffs[1].b, brightness_konst };
  GX_SetTevKColor(GX_KCOLOR2, KColor2);
  GX_SetTevKColor(GX_KCOLOR3, KColor3);

  // Stage 0
  GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K1);
  GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD2, GX_TEXMAP2, GX_COLOR0A0);
  GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_RASC, GX_CC_KONST, GX_CC_TEXC, GX_CC_ZERO);
  GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_2, GX_ENABLE, GX_TEVREG0);
  GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
  GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_RASA, GX_CA_KONST, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVREG0);
  // Stage 1
  GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);
  GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD2, GX_TEXMAP2, GX_COLOR0A0);
  GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_KONST, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
  GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_2, GX_ENABLE, GX_TEVREG1);
  GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  // Stage 2
  GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
  GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD2, GX_TEXMAP3, GX_COLOR0A0);
  GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_RASC, GX_CC_KONST, GX_CC_TEXC, GX_CC_ZERO);
  GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, GX_ENABLE, GX_TEVREG2);
  GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  // Stage 3
  GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K0);
  GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD2, GX_TEXMAP3, GX_COLOR0A0);
  GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_KONST, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
  GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  // Stage 4
  GX_SetTevKColorSel(GX_TEVSTAGE4, GX_TEV_KCSEL_K2);
  GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_KONST, GX_CC_CPREV, GX_CC_ZERO);
  GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_2, GX_DISABLE, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
  GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  // Stage 5
  GX_SetTevKColorSel(GX_TEVSTAGE5, GX_TEV_KCSEL_K2);
  GX_SetTevOrder(GX_TEVSTAGE5, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE5, GX_CC_ZERO, GX_CC_KONST, GX_CC_C2, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE5, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
  GX_SetTevAlphaOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVREG1);
  // Stage 6
  GX_SetTevKColorSel(GX_TEVSTAGE6, GX_TEV_KCSEL_K2);
  GX_SetTevOrder(GX_TEVSTAGE6, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE6, GX_CC_ZERO, GX_CC_KONST, GX_CC_C2, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE6, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevKAlphaSel(GX_TEVSTAGE6, GX_TEV_KASEL_1);
  GX_SetTevAlphaIn(GX_TEVSTAGE6, GX_CA_ZERO, GX_CA_KONST, GX_CA_A0, GX_CA_A1);
  GX_SetTevAlphaOp(GX_TEVSTAGE6, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  // Stage 7
  GX_SetTevOrder(GX_TEVSTAGE7, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE7, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE7, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevKAlphaSel(GX_TEVSTAGE7, GX_TEV_KASEL_K1_A);
  GX_SetTevAlphaIn(GX_TEVSTAGE7, GX_CA_ZERO, GX_CA_KONST, GX_CA_A1, GX_CA_APREV);
  GX_SetTevAlphaOp(GX_TEVSTAGE7, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVREG2);
  // Stage 8
  GX_SetTevKColorSel(GX_TEVSTAGE8, GX_TEV_KCSEL_1);
  GX_SetTevOrder(GX_TEVSTAGE8, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE8, GX_CC_ZERO, GX_CC_ONE, GX_CC_A2, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE8, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE8, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE8, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  // Stage 9
  GX_SetTevKColorSel(GX_TEVSTAGE9, GX_TEV_KCSEL_K3);
  GX_SetTevOrder(GX_TEVSTAGE9, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE9, GX_CC_ZERO, GX_CC_KONST, GX_CC_C1, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE9, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE9, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE9, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  // Stage 10
  GX_SetTevKColorSel(GX_TEVSTAGE10, GX_TEV_KCSEL_K3);
  GX_SetTevOrder(GX_TEVSTAGE10, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE10, GX_CC_ZERO, GX_CC_KONST, GX_CC_C1, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE10, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE10, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE10, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  // Stage 11
  GX_SetTevKColorSel(GX_TEVSTAGE11, GX_TEV_KCSEL_K3);
  GX_SetTevOrder(GX_TEVSTAGE11, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE11, GX_CC_ZERO, GX_CC_KONST, GX_CC_C0, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE11, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevAlphaIn(GX_TEVSTAGE11, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
  GX_SetTevAlphaOp(GX_TEVSTAGE11, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  // Stage 12
  GX_SetTevKColorSel(GX_TEVSTAGE12, GX_TEV_KCSEL_K3);
  GX_SetTevOrder(GX_TEVSTAGE12, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE12, GX_CC_ZERO, GX_CC_KONST, GX_CC_C0, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE12, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  GX_SetTevKAlphaSel(GX_TEVSTAGE12, GX_TEV_KASEL_K2_A);
  GX_SetTevAlphaIn(GX_TEVSTAGE12, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
  GX_SetTevAlphaOp(GX_TEVSTAGE12, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  // Stage 13
  GX_SetTevOrder(GX_TEVSTAGE13, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE13, GX_CC_ZERO, GX_CC_CPREV, GX_CC_APREV, contrast_InD);
  GX_SetTevColorOp(GX_TEVSTAGE13, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
  GX_SetTevKAlphaSel(GX_TEVSTAGE13, GX_TEV_KASEL_K3_A);
  GX_SetTevAlphaIn(GX_TEVSTAGE13, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
  GX_SetTevAlphaOp(GX_TEVSTAGE13, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  // Stage 14
  GX_SetTevOrder(GX_TEVSTAGE14, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
  GX_SetTevColorIn(GX_TEVSTAGE14, GX_CC_APREV, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
  GX_SetTevColorOp(GX_TEVSTAGE14, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
  GX_SetTevKAlphaSel(GX_TEVSTAGE14, GX_TEV_KCSEL_1);
  GX_SetTevAlphaIn(GX_TEVSTAGE14, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
  GX_SetTevAlphaOp(GX_TEVSTAGE14, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

  // vertex description / format
  GX_ClearVtxDesc();
  GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
  GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
  GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);
  GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX8);
  GX_SetVtxDesc(GX_VA_TEX2, GX_INDEX8);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
  GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX2, GX_TEX_ST, GX_F32, 0);

  GX_SetArray(GX_VA_POS, square, 3 * sizeof(s16));
  GX_SetArray(GX_VA_CLR0, colors, sizeof(GXColor));
  GX_SetArray(GX_VA_TEX0, texcoordsY, 2 * sizeof(f32));
  GX_SetArray(GX_VA_TEX1, texcoordsY, 2 * sizeof(f32));
  GX_SetArray(GX_VA_TEX2, texcoordsUV, 2 * sizeof(f32));

  // init YUV texture objects
  GX_InitTexObj(&YltexObj, Yltexture, Ylwidth, Yheight, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GX_InitTexObjLOD(&YltexObj, GX_LINEAR, GX_LINEAR, 0.0, 0.0, 0.0, GX_TRUE, GX_TRUE, GX_ANISO_4);
  GX_InitTexObj(&YrtexObj, Yrtexture, Yrwidth, Yheight, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GX_InitTexObjLOD(&YrtexObj, GX_LINEAR, GX_LINEAR, 0.0, 0.0, 0.0, GX_TRUE, GX_TRUE, GX_ANISO_4);
  GX_InitTexObj(&UtexObj, Utexture, UVwidth, UVheight, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GX_InitTexObjLOD(&UtexObj, GX_LINEAR, GX_LINEAR, 0.0, 0.0, 0.0, GX_TRUE, GX_TRUE, GX_ANISO_4);
  GX_InitTexObj(&VtexObj, Vtexture, UVwidth, UVheight, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GX_InitTexObjLOD(&VtexObj, GX_LINEAR, GX_LINEAR, 0.0, 0.0, 0.0, GX_TRUE, GX_TRUE, GX_ANISO_4);

  GX_LoadTexObj(&YltexObj, GX_TEXMAP0);
  GX_LoadTexObj(&YrtexObj, GX_TEXMAP1);
  GX_LoadTexObj(&UtexObj, GX_TEXMAP2);
  GX_LoadTexObj(&VtexObj, GX_TEXMAP3);

  GX_BeginDispList(dlist, 32);
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position1x8(0); GX_Color1x8(0); GX_TexCoord1x8(0); GX_TexCoord1x8(4); GX_TexCoord1x8(0);
    GX_Position1x8(1); GX_Color1x8(0); GX_TexCoord1x8(1); GX_TexCoord1x8(5); GX_TexCoord1x8(1);
    GX_Position1x8(2); GX_Color1x8(0); GX_TexCoord1x8(2); GX_TexCoord1x8(6); GX_TexCoord1x8(2);
    GX_Position1x8(3); GX_Color1x8(0); GX_TexCoord1x8(3); GX_TexCoord1x8(7); GX_TexCoord1x8(3);
  GX_End();
  GX_EndDispList();
}

// modelview + viewport for the full-screen (aspect-fit) quad.
// The quad is aspect-fit to the EFB (== the reference's gx_width/2, gx_height/2)
// and viewed through the fixed camera, exactly like gx_supp.c draw_scaling.
static void draw_scaling(void) {
  Mtx m, mv;
  f32 vw = (f32) Ywidth, vh = (f32) Yheight;
  f32 ew = (f32) vmode->fbWidth, eh = (f32) vmode->efbHeight;

  f32 scale = (vw / vh > ew / eh) ? (eh / vh) : (ew / vw);
  f32 qw = vw * scale;
  f32 qh = vh * scale;

  square[0] = -(s16) (qw / 2); square[1] = (s16) (qh / 2);  square[2] = 0;
  square[3] = (s16) (qw / 2);  square[4] = (s16) (qh / 2);  square[5] = 0;
  square[6] = (s16) (qw / 2);  square[7] = -(s16) (qh / 2); square[8] = 0;
  square[9] = -(s16) (qw / 2); square[10] = -(s16) (qh / 2); square[11] = 0;
  DCFlushRange(square, sizeof(square));

  memset(&view, 0, sizeof(Mtx));
  guLookAt(view, &cam.pos, &cam.up, &cam.view);
  guMtxIdentity(m);
  guMtxTransApply(m, m, 0, 0, -100);
  guMtxConcat(view, m, mv);
  GX_LoadPosMtxImm(mv, GX_PNMTX0);
  GX_SetViewport(1.0f / 24.0f, 1.0f / 24.0f, vmode->fbWidth, vmode->efbHeight, 0, 1);
}

static int yuv_cfg_w = 0, yuv_cfg_h = 0;

void gx_video_config_yuv(int width, int height) {
  if (yuv_cfg_w == width && yuv_cfg_h == height)
    return;
  yuv_cfg_w = width;
  yuv_cfg_h = height;

  int chroma_width = width / 2;
  int chroma_height = height / 2;

  Ywidth = (width + 7) & ~7;
  UVwidth = (chroma_width + 7) & ~7;

  st0 = st1 = 0; // WiiMC GX_ConfigTextureYUV (gx_supp.c:448): force a rowpitch recompute

  wl = Ywidth / 8;
  if (wl > 1024 / 8)
    wl = 1024 / 8 - 1;
  wr = (Ywidth / 8) > (1024 / 8) ? (Ywidth / 8) - (1024 / 8) + 1 : 0;

  Ylwidth = Ywidth > 1016 ? 1024 : Ywidth;
  Yrwidth = Ywidth > 1024 ? Ywidth - 1016 + 8 : 8;

  Yheight = (height + 3) & ~3;
  UVheight = (chroma_height + 3) & ~3;

  f32 YtexcoordS = (f32) width / (f32) Ywidth;
  f32 UVtexcoordS = (f32) chroma_width / (f32) UVwidth;
  f32 YtexcoordT = (f32) height / (f32) Yheight;
  f32 UVtexcoordT = (f32) chroma_height / (f32) UVheight;

  if (Ywidth <= 1024) {
    texcoordsY[2] = texcoordsY[4] = YtexcoordS;
    texcoordsY[5] = texcoordsY[7] = YtexcoordT;
    texcoordsY[8] = texcoordsY[14] = 0.0f;
  } else {
    texcoordsY[2] = texcoordsY[4] = (f32) Ywidth / 1024.0f;
    texcoordsY[5] = texcoordsY[7] = YtexcoordT;
    texcoordsY[8] = texcoordsY[14] = (-1016.0f + 8.0f) / (f32) Yrwidth;
  }
  texcoordsUV[2] = texcoordsUV[4] = UVtexcoordS;
  texcoordsUV[5] = texcoordsUV[7] = UVtexcoordT;
  DCFlushRange(texcoordsY, 16 * sizeof(f32));
  DCFlushRange(texcoordsUV, 8 * sizeof(f32));

  // Used (non-oversized) sizes, for the DCFlushRange in gx_video_draw_yuv. The
  // buffers themselves are the pre-allocated oversized MEM2 buffers (WiiMC
  // GX_AllocTextureMemory); config only sets the dimensions + texcoords.
  Yltexsize = (u32) Ylwidth * Yheight;
  Yrtexsize = (u32) Yrwidth * Yheight;
  UVtexsize = (u32) UVwidth * UVheight;

  draw_initYUV();
  draw_scaling();

  // Inverted, transposed ortho (gx_supp.c GX_StartYUV): X range = +/-efbHeight/2
  // (flipped), Y range = +/-fbWidth/2. Matches the square's centered coordinate
  // space viewed through the camera above.
  Mtx44 p;
  guOrtho(p, vmode->efbHeight / 2.0, -vmode->efbHeight / 2.0,
          -vmode->fbWidth / 2.0, vmode->fbWidth / 2.0, 10.0, 1000.0);
  GX_LoadProjectionMtx(p, GX_ORTHOGRAPHIC);

  // Non-AA EFB (RGB8, 528 lines). GX_PF_RGB565_Z16 enables MSAA and halves the
  // EFB to 640x264 (gx.h:3482) and needs GX_SetCopyFilter; rendering a 480-tall
  // frame into it overflows -- top 264 lines (55%) are correct, the rest is
  // un-resolved-AA garbage ("bottom screen garbage", hits font+video alike).
  // WiiMC uses RGB8_Z24 (gx_supp.c:726), which fits 480 and needs no copy filter.
  GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
  GX_SetCullMode(GX_CULL_NONE);
  GX_SetClipMode(GX_DISABLE);
  GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_TRUE);
  GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  GX_SetAlphaUpdate(GX_ENABLE);
  GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GX_SetColorUpdate(GX_ENABLE);
  GX_Flush();
}

#define LUMA_COPY(type) \
{ \
  type *Yldst = (type *) Yltexture - 1; \
  type *Yrdst = (type *) Yrtexture - 1; \
  type *Ysrc1 = (type *) (buffer[0]) - 1; \
  type *Ysrc2 = (type *) ((buffer[0]) + stride[0]) - 1; \
  type *Ysrc3 = (type *) ((buffer[0]) + (stride[0] * 2)) - 1; \
  type *Ysrc4 = (type *) ((buffer[0]) + (stride[0] * 3)) - 1; \
  Yrdst += 4; \
  int rows = Yheight / 4; \
  int tiles; \
  while (rows--) { \
    tiles = wl; \
    while (tiles--) { \
      __asm__ volatile("dcbz 0,%0" : : "b" (++Yldst)); \
      *Yldst = *++Ysrc1; \
      *++Yldst = *++Ysrc2; \
      *++Yldst = *++Ysrc3; \
      *++Yldst = *++Ysrc4; \
    } \
    if (wr > 0) { \
      tiles = wr; \
      while (tiles--) { \
        __asm__ volatile("dcbz 0,%0" : : "b" (++Yrdst)); \
        *Yrdst = *++Ysrc1; \
        *++Yrdst = *++Ysrc2; \
        *++Yrdst = *++Ysrc3; \
        *++Yrdst = *++Ysrc4; \
      } \
      Yldst += 4; \
      Yrdst += 4; \
    } \
    Ysrc1 = (type *) ((u32) Ysrc1 + Yrowpitch); \
    Ysrc2 = (type *) ((u32) Ysrc2 + Yrowpitch); \
    Ysrc3 = (type *) ((u32) Ysrc3 + Yrowpitch); \
    Ysrc4 = (type *) ((u32) Ysrc4 + Yrowpitch); \
  } \
}

#define CHROMA_COPY(type) \
{ \
  type *Udst = (type *) Utexture - 1; \
  type *Vdst = (type *) Vtexture - 1; \
  type *Usrc1 = (type *) (buffer[1]) - 1; \
  type *Usrc2 = (type *) ((buffer[1]) + stride[1]) - 1; \
  type *Usrc3 = (type *) ((buffer[1]) + (stride[1] * 2)) - 1; \
  type *Usrc4 = (type *) ((buffer[1]) + (stride[1] * 3)) - 1; \
  type *Vsrc1 = (type *) (buffer[2]) - 1; \
  type *Vsrc2 = (type *) ((buffer[2]) + stride[2]) - 1; \
  type *Vsrc3 = (type *) ((buffer[2]) + (stride[2] * 2)) - 1; \
  type *Vsrc4 = (type *) ((buffer[2]) + (stride[2] * 3)) - 1; \
  int rows = UVheight / 4; \
  int tiles, ntiles = UVwidth / 8; \
  while (rows--) { \
    tiles = ntiles; \
    while (tiles--) { \
      __asm__ volatile("dcbz 0,%0" : : "b" (++Udst)); \
      *Udst = *++Usrc1; \
      *++Udst = *++Usrc2; \
      *++Udst = *++Usrc3; \
      *++Udst = *++Usrc4; \
      __asm__ volatile("dcbz 0,%0" : : "b" (++Vdst)); \
      *Vdst = *++Vsrc1; \
      *++Vdst = *++Vsrc2; \
      *++Vdst = *++Vsrc3; \
      *++Vdst = *++Vsrc4; \
    } \
    Usrc1 = (type *) ((u32) Usrc1 + UVrowpitch); \
    Usrc2 = (type *) ((u32) Usrc2 + UVrowpitch); \
    Usrc3 = (type *) ((u32) Usrc3 + UVrowpitch); \
    Usrc4 = (type *) ((u32) Usrc4 + UVrowpitch); \
    Vsrc1 = (type *) ((u32) Vsrc1 + UVrowpitch); \
    Vsrc2 = (type *) ((u32) Vsrc2 + UVrowpitch); \
    Vsrc3 = (type *) ((u32) Vsrc3 + UVrowpitch); \
    Vsrc4 = (type *) ((u32) Vsrc4 + UVrowpitch); \
  } \
}

static void GX_FillTextureYUV(u8* buffer[3], int stride[3]) {
  if (st0 != stride[0] || st1 != stride[1]) {
    st0 = stride[0];
    st1 = stride[1];
    Yrowpitch = (stride[0] * 4) - Ywidth;
    UVrowpitch = (stride[1] * 4) - UVwidth;
  }

  if (stride[0] & 7)
    LUMA_COPY(u64)
  else
    LUMA_COPY(double)

  if (stride[1] & 7)
    CHROMA_COPY(u64)
  else
    CHROMA_COPY(double)
}

void gx_video_draw_yuv(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                       int ystride, int uvstride) {
  u8* buffer[3] = { (u8*) y, (u8*) u, (u8*) v };
  int stride[3] = { ystride, uvstride, uvstride };

  // Wait for the GPU to finish the previous frame BEFORE overwriting the shared
  // Y/UV texture (WiiMC's GX_FillTextureYUV gates its fill on need_wait). Filling
  // while the GPU is still sampling the texture is a race that shows up as
  // content-dependent noise.
  if (gx_presented)
    GX_WaitDrawDone();

  GX_FillTextureYUV(buffer, stride);

  DCFlushRange(Yltexture, Yltexsize);
  if (wr > 0)
    DCFlushRange(Yrtexture, Yrtexsize);
  DCFlushRange(Utexture, UVtexsize);
  DCFlushRange(Vtexture, UVtexsize);

  GX_InvVtxCache();
  GX_InvalidateTexAll();
  // Pace the render to the VI flip (WiiMC style): one retrace if a flip is
  // still pending.
  if (flip_pending)
    VIDEO_WaitVSync();

  GX_CallDispList(dlist, 32);

  gx_video_present();

  gx_video_dbg_maybe();
}

// Render a full-screen grayscale (Y) image (the font) through the YUV pipeline:
// neutral U/V so the TEV outputs Y as RGB. Mirrors gx_supp.c's vo_draw_alpha_gekko
// (text into the Y plane) rather than a standalone RGBA8 texture.
void gx_video_draw_gray(int width, int height, const uint8_t* gray, int gray_stride) {
  static uint8_t* neutral_uv = NULL;
  static int neutral_uv_size = 0;

  int uv_w = (width + 1) / 2;
  int uv_h = (height + 1) / 2;
  int need = uv_w * uv_h;
  if (neutral_uv_size < need) {
    free(neutral_uv);
    neutral_uv = (uint8_t*) memalign(32, need);
    neutral_uv_size = need;
  }
  memset(neutral_uv, 0x80, need);

  gx_video_config_yuv(width, height);
  gx_video_draw_yuv(gray, neutral_uv, neutral_uv, gray_stride, uv_w);
}
