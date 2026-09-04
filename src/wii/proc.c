// Homebrew on the Wii has no system-level home button menu; the Wiimote
// home button is handled in input.c (long press stops the stream).

#include "wii.h"

#include <stdlib.h>

static int running = 0;
static int want_main_menu = 0;
int homeEnabled = 1;

void wii_proc_init(void) {
  running = 1;
}

void wii_proc_shutdown(void) {
  running = 0;
}

void wii_proc_register_home_callback(void) {
}

int wii_proc_running(void) {
  return running;
}

void wii_proc_stop_running(void) {
  running = 0;
}

void wii_proc_set_home_enabled(int enabled) {
  homeEnabled = enabled;
}

void wii_proc_set_want_main_menu(int v) {
  want_main_menu = v;
}

int wii_proc_want_main_menu(void) {
  return want_main_menu;
}
