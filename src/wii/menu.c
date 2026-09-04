// On-screen menu framework for the Wii (Wiimote D-pad + A/B, no keyboard).
#include "menu.h"
#include "wii.h"

#include <wiiuse/wpad.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>

#define MENU_GREEN  120, 255, 120, 255
#define MENU_WHITE  235, 235, 235, 255
#define MENU_DIM    150, 150, 150, 255

static int charset_index(const char* charset, char c) {
  for (int i = 0; charset[i] != '\0'; i++)
    if (charset[i] == c)
      return i;
  return 0;
}

// Fill any gap between the existing length and the cursor with the first
// charset character so the buffer never contains embedded NULs.
static void extend_to(char* b, int* l, int p, const char* cs) {
  while (*l <= p) {
    b[*l] = cs[0];
    (*l)++;
  }
}

static void draw_help(const char* help) {
  Font_SetColor(MENU_DIM);
  Font_SetSize(18);
  Font_Print(8, 452, help);
}

// Render the static parts of a selection list (title + every option + help)
// with no highlight. Done once per menu; the result is cached so that each
// navigation step only re-renders the single highlighted line instead of the
// whole screen (freetype is the expensive part on the Wii).
static void menu_render_base(const char* title, const char** options, int count) {
  Font_Clear();
  Font_SetColor(MENU_WHITE);
  Font_SetSize(28);
  Font_Print(8, 24, title);

  Font_SetSize(24);
  for (int i = 0; i < count; i++) {
    Font_SetColor(MENU_WHITE);
    Font_Print(56, 96 + i * 40, options[i]);
  }

  draw_help("Up/Down: select   A: ok   B: back");
}

int menu_select(const char* title, const char** options, int count, int sel) {
  if (count <= 0)
    return -1;
  if (sel < 0)
    sel = 0;

  uint8_t* fb = Font_Buffer();
  uint32_t fsz = Font_BufferSize();

  static uint8_t* base = NULL;
  static uint32_t base_size = 0;
  if (base == NULL || base_size < fsz) {
    free(base);
    base = (uint8_t*) memalign(64, fsz);
    base_size = fsz;
  }

  // one full render, then cache the whole screen
  menu_render_base(title, options, count);
  memcpy(base, fb, fsz);

  int last = -1;
  while (wii_proc_running()) {
    if (wii_proc_want_main_menu())
      return -1;
    if (sel < 0)
      sel = 0;
    if (sel >= count)
      sel = count - 1;

    // Re-render the highlighted line only when it changes (freetype is the
    // expensive part), but present every frame. The present is now
    // non-blocking (the VSync flip happens in a callback, see gx_video.c), so
    // the loop is no longer throttled to the VSync rate and fast D-pad presses
    // are no longer dropped.
    if (sel != last) {
      memcpy(fb, base, fsz);
      Font_SetColor(MENU_GREEN);
      Font_SetSize(24);
      Font_PrintChar(16, 96 + sel * 40, '>');
      Font_Print(56, 96 + sel * 40, options[sel]);
      last = sel;
      // present only when the content actually changed
      Font_Draw_TVDRC();
    }

    uint32_t btns = wii_input_buttons_triggered();
    if (btns & WPAD_BUTTON_A)
      return sel;
    if (btns & WPAD_BUTTON_B)
      return -1;
    if (btns & WPAD_BUTTON_DOWN)
      sel++;
    else if (btns & WPAD_BUTTON_UP)
      sel--;

    usleep(5000);
  }
  return -1;
}

int menu_input_string(const char* title, const char* charset,
                      char* out, int maxlen, const char* initial) {
  int n = (int) strlen(charset);
  char buf[256];
  if (n == 0 || maxlen <= 0 || maxlen >= (int) sizeof(buf))
    return 0;

  memset(buf, 0, sizeof(buf));
  if (initial) {
    strncpy(buf, initial, maxlen);
    buf[maxlen] = '\0';
  }
  int len = (int) strlen(buf);
  int pos = 0;
  int sel = charset_index(charset, buf[0]);

  int last_pos = -1, last_sel = -1, last_len = -1;
  while (wii_proc_running()) {
    if (wii_proc_want_main_menu())
      return 0;
    if (pos < 0)
      pos = 0;
    if (pos > maxlen - 1)
      pos = maxlen - 1;
    if (sel < 0)
      sel = 0;
    if (sel >= n)
      sel = n - 1;

    if (pos != last_pos || sel != last_sel || len != last_len) {
      Font_Clear();
      Font_SetColor(MENU_WHITE);
      Font_SetSize(28);
      Font_Print(8, 24, title);

      // the value being entered, cursor position highlighted
      Font_SetSize(30);
      int dmax = (maxlen < 16) ? maxlen : 16;
      for (int i = 0; i < dmax; i++) {
        int x = 8 + i * 26;
        if (i == pos)
          Font_SetColor(MENU_GREEN);
        else if (i < len)
          Font_SetColor(MENU_WHITE);
        else
          Font_SetColor(MENU_DIM);
        char c = (i < len) ? buf[i] : '.';
        Font_PrintChar(x, 120, c);
      }

      // the character set, selected entry highlighted
      Font_SetSize(22);
      for (int i = 0; i < n; i++) {
        int y = 60 + i * 24;
        if (i == sel)
          Font_SetColor(MENU_GREEN);
        else
          Font_SetColor(MENU_DIM);
        Font_PrintChar(560, y, charset[i]);
      }

      draw_help("Up/Down: char   L/R: move   A: done   B: cancel");
      last_pos = pos;
      last_sel = sel;
      last_len = len;
      // present only when the content actually changed (see menu_select)
      Font_Draw_TVDRC();
    }

    uint32_t btns = wii_input_buttons_triggered();
    if (btns & WPAD_BUTTON_A) {
      buf[len] = '\0';
      strncpy(out, buf, maxlen);
      out[maxlen] = '\0';
      return 1;
    }
    if (btns & WPAD_BUTTON_B)
      return 0;
    if (btns & WPAD_BUTTON_UP) {
      sel = (sel - 1 + n) % n;
      extend_to(buf, &len, pos, charset);
      buf[pos] = charset[sel];
    } else if (btns & WPAD_BUTTON_DOWN) {
      sel = (sel + 1) % n;
      extend_to(buf, &len, pos, charset);
      buf[pos] = charset[sel];
    } else if (btns & WPAD_BUTTON_LEFT) {
      if (pos > 0) {
        pos--;
        sel = charset_index(charset, buf[pos]);
      }
    } else if (btns & WPAD_BUTTON_RIGHT) {
      if (pos < maxlen - 1) {
        pos++;
        if (pos >= len)
          sel = 0;
        else
          sel = charset_index(charset, buf[pos]);
      }
    }

    usleep(5000);
  }
  return 0;
}

int menu_input_int(const char* title, int initial) {
  char buf[16];
  char out[16];
  snprintf(buf, sizeof(buf), "%d", initial < 0 ? 0 : initial);

  if (!menu_input_string(title, "0123456789", out, 15, buf))
    return -1;

  int v = atoi(out);
  if (v < 0)
    v = 0;
  return v;
}

int menu_select_bool(const char* title, int initial) {
  const char* opts[2] = { "Yes", "No" };
  int r = menu_select(title, opts, 2, initial ? 0 : 1);
  if (r < 0)
    return -1;
  return r == 0;
}
