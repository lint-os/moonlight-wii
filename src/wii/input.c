#include "../config.h"
#include "wii.h"

#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/time.h>
#include <wiiuse/wpad.h>

int autostream = 0;

extern int homeEnabled;

static uint64_t micros_now(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}

static uint64_t homeDownMicros = 0;
static int homeHeld = 0;
static int homeLongFired = 0;

#define HOME_HOLD_MICROS (3 * 1000 * 1000)

// Home button actions.
#define HOME_NONE 0
#define HOME_BACK 1   // short press: back to the main menu
#define HOME_QUIT 2   // long press: exit the program

// Track the home button level across polls. Releasing before HOME_HOLD_MICROS is
// a short press (BACK); holding past it fires QUIT once. Called once per
// WPAD_ScanPads, from either the input thread (while streaming) or the menu
// poller (wii_input_buttons_triggered) -- never both at once.
static int home_poll(u32 btns) {
  int action = HOME_NONE;
  if (btns & WPAD_BUTTON_HOME) {
    if (!homeHeld) {
      homeHeld = 1;
      homeLongFired = 0;
      homeDownMicros = micros_now();
    } else if (!homeLongFired && micros_now() - homeDownMicros > HOME_HOLD_MICROS) {
      homeLongFired = 1;
      action = HOME_QUIT;
    }
  } else {
    if (homeHeld && !homeLongFired)
      action = HOME_BACK;
    homeHeld = 0;
    homeLongFired = 0;
  }
  return action;
}

static void send_controller_event(WPADData* wiimote) {
  short buttonFlags = 0;
  u32 btns = wiimote->btns_l | wiimote->btns_h;

#define CHECKBTN(v, f) if (btns & (v)) buttonFlags |= (f);
  CHECKBTN(WPAD_BUTTON_A,     A_FLAG);
  CHECKBTN(WPAD_BUTTON_B,     B_FLAG);
  CHECKBTN(WPAD_BUTTON_1,     LB_FLAG);
  CHECKBTN(WPAD_BUTTON_2,     RB_FLAG);
  CHECKBTN(WPAD_BUTTON_UP,    UP_FLAG);
  CHECKBTN(WPAD_BUTTON_DOWN,  DOWN_FLAG);
  CHECKBTN(WPAD_BUTTON_LEFT,  LEFT_FLAG);
  CHECKBTN(WPAD_BUTTON_RIGHT, RIGHT_FLAG);
  CHECKBTN(WPAD_BUTTON_PLUS,  PLAY_FLAG);
  CHECKBTN(WPAD_BUTTON_MINUS, BACK_FLAG);
#undef CHECKBTN

  int16_t stickX = 0;
  int16_t stickY = 0;
  unsigned char rightTrigger = 0;

  if (wiimote->exp.type == WPAD_EXP_NUNCHUK) {
    nunchuk_t* nunchuk = &wiimote->exp.nunchuk;
    if (nunchuk->btns_held & NUNCHUK_BUTTON_Z)
      rightTrigger = 0xFF;
    if (nunchuk->btns_held & NUNCHUK_BUTTON_C)
      buttonFlags |= Y_FLAG;
    if (nunchuk->btns_held & NUNCHUK_BUTTON_Z)
      buttonFlags |= X_FLAG;

    stickX = (int16_t) ((int16_t) nunchuk->js.pos.x - (int16_t) nunchuk->js.center.x) * 2;
    stickY = (int16_t) ((int16_t) nunchuk->js.pos.y - (int16_t) nunchuk->js.center.y) * 2;
  }

  LiSendMultiControllerEvent(0, 0x1, buttonFlags, 0, rightTrigger,
    stickX, stickY, 0, 0);
}

void wii_input_init(void) {
  WPAD_Init();
}

void wii_input_update(void) {
  WPAD_ScanPads();

  WPADData* wiimote = WPAD_Data(WPAD_CHAN_0);
  if (wiimote->err != WPAD_ERR_NONE || !wiimote->data_present)
    return;

  u32 btns = wiimote->btns_l | wiimote->btns_h;

  if (homeEnabled) {
    int action = home_poll(btns);
    if (action == HOME_QUIT) {
      wii_proc_stop_running();
      return;
    }
    if (action == HOME_BACK) {
      state = STATE_STOP_STREAM;
      return;
    }
  }

  send_controller_event(wiimote);
}

uint32_t wii_input_num_controllers(void) {
  WPAD_ScanPads();

  WPADData* wiimote = WPAD_Data(WPAD_CHAN_0);
  if (wiimote->err == WPAD_ERR_NONE && wiimote->data_present)
    return 1;

  return 0;
}

uint32_t wii_input_buttons_triggered(void) {
  WPAD_ScanPads();

  WPADData* wiimote = WPAD_Data(WPAD_CHAN_0);
  if (wiimote->err != WPAD_ERR_NONE || !wiimote->data_present)
    return 0;

  u32 btns = 0;
  u32 down = wiimote->btns_d;

  if (homeEnabled) {
    u32 level = wiimote->btns_l | wiimote->btns_h;
    int action = home_poll(level);
    if (action == HOME_QUIT)
      wii_proc_stop_running();
    else if (action == HOME_BACK)
      wii_proc_set_want_main_menu(1);
  }

#define MAPBTNS(v) if (down & (v)) btns |= (v);
  MAPBTNS(WPAD_BUTTON_A);
  MAPBTNS(WPAD_BUTTON_B);
  MAPBTNS(WPAD_BUTTON_1);
  MAPBTNS(WPAD_BUTTON_2);
  MAPBTNS(WPAD_BUTTON_UP);
  MAPBTNS(WPAD_BUTTON_DOWN);
  MAPBTNS(WPAD_BUTTON_LEFT);
  MAPBTNS(WPAD_BUTTON_RIGHT);
  MAPBTNS(WPAD_BUTTON_PLUS);
  MAPBTNS(WPAD_BUTTON_MINUS);
  MAPBTNS(WPAD_BUTTON_HOME);
#undef MAPBTNS

  if (wiimote->exp.type == WPAD_EXP_NUNCHUK) {
    if (wiimote->exp.nunchuk.btns & NUNCHUK_BUTTON_Z)
      btns |= WPAD_BUTTON_2;
    if (wiimote->exp.nunchuk.btns & NUNCHUK_BUTTON_C)
      btns |= WPAD_BUTTON_1;
  }

  return btns;
}

static int input_thread_running;
static pthread_t inputThread;

static void* input_thread_proc(void* arg) {
  while (input_thread_running) {
    wii_input_update();
    usleep(10000); // 100 Hz
  }
  return NULL;
}

void start_input_thread(void) {
  input_thread_running = 1;
  if (pthread_create(&inputThread, NULL, input_thread_proc, NULL) != 0)
    input_thread_running = 0;
}

void stop_input_thread(void) {
  input_thread_running = 0;
  pthread_join(inputThread, NULL);
}
