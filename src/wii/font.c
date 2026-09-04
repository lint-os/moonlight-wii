// A simple software font renderer using freetype, drawing straight into the EFB
#include "font.h"
#include "wii.h"
#include "gx_video.h"

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include <ft2build.h>
#include FT_FREETYPE_H

extern unsigned char font_ttf[];
extern unsigned int font_ttf_len;

static FT_Library ft_lib = NULL;
static FT_Face ft_face = NULL;

static uint8_t* font_buffer;
static uint32_t font_width;
static uint32_t font_height;
static uint32_t font_pitch;

static uint8_t font_luma = 255, font_a = 255;
static const uint8_t bg_luma = 45;

// Overscan margin (px) inset on every side so OSD text stays inside the TV's
// safe area. 0 = disabled.
static uint32_t font_overscan = 0;

void Font_SetOverscan(uint32_t margin) {
  font_overscan = margin;
}

// Map a position in the full EFB canvas into the safe area (inset by the
// overscan margin). Keeps the centre fixed and pulls the edges inward.
static void font_overscan_map(int* px, int* py) {
  if (font_overscan <= 0)
    return;
  int m = (int) font_overscan;
  int w = (int) font_width, h = (int) font_height;
  int sw = w - 2 * m, sh = h - 2 * m;
  if (sw < 1)
    sw = 1;
  if (sh < 1)
    sh = 1;
  *px = m + *px * sw / w;
  *py = m + *py * sh / h;
}

void Font_Init(void) {
  FT_Init_FreeType(&ft_lib);
  FT_New_Memory_Face(ft_lib, font_ttf, font_ttf_len, 0, &ft_face);
  FT_Set_Pixel_Sizes(ft_face, 0, 24);

  font_width = wii_efb_width();
  font_height = wii_efb_height();
  font_pitch = font_width;
  font_buffer = memalign(64, font_pitch * font_height);
  if (font_buffer == NULL) {
    fprintf(stderr, "Not enough memory for font buffer\n");
    exit(-1);
  }

  Font_Clear();
}

void Font_Deinit(void) {
  FT_Done_Face(ft_face);
  FT_Done_FreeType(ft_lib);

  free(font_buffer);
  font_buffer = NULL;
}

void Font_Draw(void) {
  gx_video_draw_gray(font_width, font_height, font_buffer, font_pitch);
}

void Font_Draw_TVDRC(void) {
  Font_Draw();
}

void Font_Clear(void) {
  memset(font_buffer, bg_luma, font_width * font_height);
}

static void draw_freetype_bitmap(FT_Bitmap* bitmap, FT_Int x, FT_Int y) {
  FT_Int i, j, p, q;
  FT_Int x_max = x + bitmap->width;
  FT_Int y_max = y + bitmap->rows;

  for (i = x, p = 0; i < x_max; i++, p++) {
    for (j = y, q = 0; j < y_max; j++, q++) {
      if (i < 0 || j < 0 || i >= (FT_Int) font_width || j >= (FT_Int) font_height) {
        continue;
      }

      uint8_t opacity = bitmap->buffer[q * bitmap->pitch + p];
      if (opacity == 0) {
        continue;
      }

      uint8_t* dst = font_buffer + (j * font_pitch + i);
      uint8_t a = font_a * opacity / 255;
      *dst = (font_luma * a + *dst * (255 - a)) / 255;
    }
  }
}

void Font_Printw(uint32_t x, uint32_t y, const wchar_t* string) {
  if (ft_face == NULL)
    return;

  int ox = (int) x, oy = (int) y;
  font_overscan_map(&ox, &oy);

  FT_GlyphSlot slot = ft_face->glyph;
  FT_Vector pen = { ox, oy };

  for (; *string; string++) {
    uint32_t charcode = *string;

    if (charcode == '\n') {
      pen.y += ft_face->size->metrics.height >> 6;
      pen.x = ox;
      continue;
    }

    FT_Load_Glyph(ft_face, FT_Get_Char_Index(ft_face, charcode), FT_LOAD_DEFAULT);
    FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);

    draw_freetype_bitmap(&slot->bitmap, pen.x + slot->bitmap_left, pen.y - slot->bitmap_top);
    pen.x += slot->advance.x >> 6;
  }
}

void Font_Print(uint32_t x, uint32_t y, const char* string) {
  wchar_t* buffer = calloc(strlen(string) + 1, sizeof(wchar_t));

  size_t num = mbstowcs(buffer, string, strlen(string));
  if (num > 0) {
    buffer[num] = 0;
  }
  else {
    wchar_t* tmp = buffer;
    while ((*tmp++ = *string++));
  }

  Font_Printw(x, y, buffer);
  free(buffer);
}

void Font_Printf(uint32_t x, uint32_t y, const char* msg, ...) {
  va_list args;
  va_start(args, msg);

  char* tmp = NULL;
  if ((vasprintf(&tmp, msg, args) >= 0) && tmp) {
    Font_Print(x, y, tmp);
  }

  va_end(args);
  free(tmp);
}

void Font_SetColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  font_luma = (r * 77 + g * 150 + b * 29) / 256;
  font_a = a;
}

void Font_SetSize(uint32_t size) {
  if (ft_face == NULL)
    return;
  FT_Set_Pixel_Sizes(ft_face, 0, size);
}

uint8_t* Font_Buffer(void) {
  return font_buffer;
}

uint32_t Font_BufferSize(void) {
  return font_width * font_height;
}

// Render a single (ASCII) character without the wchar_t conversion that
// Font_Print does per call -- used for per-character menu drawing.
void Font_PrintChar(uint32_t x, uint32_t y, char c) {
  wchar_t w[2] = { (wchar_t) (unsigned char) c, 0 };
  Font_Printw(x, y, w);
}
