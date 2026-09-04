/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2019 Iwan Timmer
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

#include "connection.h"
#include "config.h"

#include <Limelight.h>

#include <client.h>
#include <errors.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "wii/wii.h"
#include "wii/gx_video.h"
#include "wii/menu.h"
#include <wiiuse/wpad.h>

#ifdef DEBUG
void Debug_Init();
#endif

int state = STATE_INVALID;

extern int autostream;
extern unsigned long long gettime(void);

int is_error = 0;
char message_buffer[1024] = "\0";

static void show_error(const char* msg) {
  fprintf(stderr, "%s\n", msg);
  snprintf(message_buffer, sizeof(message_buffer), "%s", msg);
  is_error = 1;
}

// Log the clocks the app can see. On the Wii, libc time() is timebase-based
// (2000 + uptime, NOT the RTC) while SYS_Time() is the RTC-based clock
// (ticks since 2000, 0 offset if the RTC read failed at boot). mkcert.c
// stamps the client cert validity with time(), so compare these on Dolphin
// vs real HW to see what dates the cert actually gets.
static void log_clocks(void) {
  char buf[32];
  struct tm tm;

  time_t now = time(NULL);
  if (now >= 0 && gmtime_r(&now, &tm) != NULL) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    printf("clock libc time(): %s (epoch %ld)\n", buf, (long) now);
  } else {
    printf("clock libc time(): FAILED (epoch %ld)\n", (long) now);
  }

  u64 rtc = SYS_Time();
  time_t rtcNow = (time_t) (946684800ull + ticks_to_secs(rtc));
  if (gmtime_r(&rtcNow, &tm) != NULL) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    printf("clock SYS_Time():  %s (ticks since 2000: %llu)\n",
           buf, (unsigned long long) rtc);
  } else {
    printf("clock SYS_Time():  FAILED (ticks since 2000: %llu)\n",
           (unsigned long long) rtc);
  }

  printf("clock gettime():  %llu ticks (%llu s uptime)\n",
         (unsigned long long) gettime(),
         (unsigned long long) (gettime() / PPC_TIMER_CLOCK));
}

// Draw the current status line (green, or red on error) under the header.
static void draw_status(const char* header) {
  Font_Clear();
  Font_SetColor(255, 255, 255, 255);
  Font_SetSize(24);
  Font_Print(8, 20, header);

  Font_SetColor(is_error ? 255 : 0, is_error ? 0 : 255, 0, 255);
  Font_SetSize(24);
  Font_Print(8, 200, message_buffer);
  Font_Draw_TVDRC();
}

static void wait_for_button(void) {
  Font_SetColor(150, 150, 150, 255);
  Font_SetSize(18);
  Font_Print(8, 452, "Press A to continue");
  Font_Draw_TVDRC();
  while (wii_proc_running()) {
    if (wii_input_buttons_triggered() & WPAD_BUTTON_A)
      break;
    if (wii_proc_want_main_menu())
      break;
    usleep(10000);
  }
}

static GS_CLIENT create_client(PCONFIGURATION config) {
  Font_Clear();
  Font_SetColor(255, 255, 255, 255);
  Font_SetSize(24);
  Font_Print(8, 20, "Setting up client...");
  Font_Draw_TVDRC();

  GS_CLIENT client = gs_new(config->key_dir);
  if (client == NULL) {
    if (gs_conf_init(config->key_dir) != GS_OK) {
      char msg[512];
      snprintf(msg, sizeof(msg), "Failed to create client info:\n%s.", gs_get_error_message());
      show_error(msg);
      draw_status(msg);
      return NULL;
    }
    client = gs_new(config->key_dir);
  }

  if (client == NULL) {
    char msg[512];
    snprintf(msg, sizeof(msg), "Failed to create GameStream client:\n%s.", gs_get_error_message());
    show_error(msg);
    draw_status(msg);
  }
  return client;
}

static void save_config(PCONFIGURATION config) {
  char path[256];
  snprintf(path, sizeof(path), "%s/moonlight.conf", MOONLIGHT_WII_PATH);
  config_save(path, config);
}

// Enter (or edit) the host address one character at a time with the D-pad.
static int enter_address(PCONFIGURATION config) {
  char addr[16];
  const char* initial = config->address ? config->address : "";

  if (!menu_input_string("Enter IP address", "0123456789.", addr, 15, initial))
    return 0;

  int n = (int) strlen(addr);
  while (n > 0 && (addr[n - 1] == '.' || addr[n - 1] == ' '))
    addr[--n] = '\0';

  if (n == 0)
    return 0;

  if (config->address)
    free(config->address);
  config->address = strdup(addr);
  save_config(config);
  return 1;
}

static void settings_menu(PCONFIGURATION config) {
  const char* opts[] = {
    "Resolution",
    "FPS",
    "Bitrate (kbps)",
    "View only (no input)",
    "Local audio",
    "Quit app after stream",
    "OSD overscan (px)",
    "Encrypt stream",
  };
  const int n = (int) (sizeof(opts) / sizeof(opts[0]));

  while (wii_proc_running()) {
    int sel = menu_select("Stream settings", opts, n, 0);
    if (sel < 0)
      break;

    switch (sel) {
      case 0: {
        const char* res[] = { "640x480", "854x480", "1280x720", "1920x1080" };
        static const int w[4] = { 640, 854, 1280, 1920 };
        static const int h[4] = { 480, 480, 720, 1080 };
        int cur = 0;
        if (config->stream.width == 854) cur = 1;
        else if (config->stream.width == 1280) cur = 2;
        else if (config->stream.width == 1920) cur = 3;
        int r = menu_select("Resolution", res, 4, cur);
        if (r >= 0) {
          config->stream.width = w[r];
          config->stream.height = h[r];
        }
        break;
      }
      case 1: {
        int v = menu_input_int("FPS", config->stream.fps);
        if (v > 0) config->stream.fps = v;
        break;
      }
      case 2: {
        int v = menu_input_int("Bitrate (kbps)", config->stream.bitrate);
        if (v > 0) config->stream.bitrate = v;
        break;
      }
      case 3: {
        int v = menu_select_bool("Disable controller input", config->viewonly);
        if (v >= 0) config->viewonly = v;
        break;
      }
      case 4: {
        int v = menu_select_bool("Enable local audio", config->localaudio);
        if (v >= 0) config->localaudio = v;
        break;
      }
      case 5: {
        int v = menu_select_bool("Quit app after streaming", config->quitappafter);
        if (v >= 0) config->quitappafter = v;
        break;
      }
      case 6: {
        int v = menu_input_int("OSD overscan (px)", config->osd_overscan);
        if (v >= 0) {
          config->osd_overscan = v;
          Font_SetOverscan(v);
        }
        break;
      }
      case 7: {
        const char* enc[] = { "None", "Audio", "Video", "All" };
        static const int encvals[4] = { ENCFLG_NONE, ENCFLG_AUDIO, ENCFLG_VIDEO,
                                        ENCFLG_AUDIO | ENCFLG_VIDEO };
        int f = config->stream.encryptionFlags;
        int cur = 0;
        if ((f & ENCFLG_VIDEO) && (f & ENCFLG_AUDIO))
          cur = 3;
        else if (f & ENCFLG_VIDEO)
          cur = 2;
        else if (f & ENCFLG_AUDIO)
          cur = 1;
        int r = menu_select("Encrypt stream", enc, 4, cur);
        if (r >= 0)
          config->stream.encryptionFlags = encvals[r];
        break;
      }
    }
  }

  save_config(config);
}

static int connect_server(GS_CLIENT client, PSERVER_DATA server, PCONFIGURATION config) {
  char header[128];
  snprintf(header, sizeof(header), "Connecting to %s...", config->address);
  draw_status(header);

  int ret = gs_get_status(client, server, config->address, config->unsupported);
  if (ret == GS_OUT_OF_MEMORY) {
    show_error("Not enough memory");
  } else if (ret == GS_ERROR) {
    char m[512];
    snprintf(m, sizeof(m), "Gamestream error:\n%s", gs_get_error_message());
    show_error(m);
  } else if (ret == GS_INVALID) {
    char m[512];
    snprintf(m, sizeof(m), "Invalid data from server:\n%s", gs_get_error_message());
    show_error(m);
  } else if (ret == GS_UNSUPPORTED_VERSION) {
    char m[512];
    snprintf(m, sizeof(m), "Unsupported version:\n%s", gs_get_error_message());
    show_error(m);
  } else if (ret != GS_OK) {
    show_error("Can't connect to server");
  } else {
    is_error = 0;
    if (config->debug_level > 0) {
      printf("NVIDIA %s, GFE %s (%s, %s)\n", server->gpuType,
             server->serverInfo.serverInfoGfeVersion, server->gsVersion,
             server->serverInfo.serverInfoAppVersion);
    }
    return 0;
  }

  draw_status("Connection failed");
  wait_for_button();
  return -1;
}

static void do_pair(GS_CLIENT client, PSERVER_DATA server) {
  char pin[5];
  srandom((unsigned) gettime());
  sprintf(pin, "%d%d%d%d", (unsigned)random() % 10, (unsigned)random() % 10,
          (unsigned)random() % 10, (unsigned)random() % 10);
  printf("Please enter the following PIN on the target PC: %s\n", pin);

  Font_Clear();
  Font_SetColor(255, 255, 255, 255);
  Font_SetSize(24);
  Font_Printf(8, 20, "Enter this PIN on the target PC:\n\n    %s\n", pin);
  Font_Draw_TVDRC();

  gs_set_timeout(client, 60);
  bool ok = (gs_pair(client, server, &pin[0]) == GS_OK);
  gs_set_timeout(client, 5);

  if (ok) {
    sprintf(message_buffer, "Succesfully paired\n");
    is_error = 0;
  } else {
    show_error("Failed to pair to server");
  }
  draw_status(ok ? "Pairing" : "Pairing failed");
  wait_for_button();
}

static void do_unpair(GS_CLIENT client, PSERVER_DATA server) {
  bool ok = (gs_unpair(client, server) == GS_OK);
  if (ok) {
    sprintf(message_buffer, "Succesfully unpaired\n");
    is_error = 0;
  } else {
    show_error("Failed to unpair from server");
  }
  draw_status(ok ? "Unpairing" : "Unpairing failed");
  wait_for_button();
}

static void free_app_list(PAPP_LIST list) {
  while (list) {
    PAPP_LIST next = list->next;
    free(list->name);
    free(list);
    list = next;
  }
}

static int find_app_id(PAPP_LIST list, const char* name) {
  while (list) {
    if (list->name && strcmp(list->name, name) == 0)
      return list->id;
    list = list->next;
  }
  return -1;
}

static int get_app_id(GS_CLIENT client, PSERVER_DATA server, const char* name) {
  PAPP_LIST list = NULL;
  if (gs_applist(client, server, &list) != GS_OK) {
    fprintf(stderr, "Can't get app list\n");
    return -1;
  }
  int id = find_app_id(list, name);
  free_app_list(list);
  return id;
}

static void do_stream(GS_CLIENT client, PSERVER_DATA server, PCONFIGURATION config) {
  int appId = -1;
  int haveList = 0;

  PAPP_LIST list = NULL;
  if (gs_applist(client, server, &list) == GS_OK && list != NULL) {
    haveList = 1;

    const char* names[128];
    int count = 0;
    for (PAPP_LIST it = list; it != NULL && count < (int)(sizeof(names) / sizeof(names[0])); it = it->next)
      names[count++] = it->name;

    if (count > 0) {
      int sel = menu_select("Select app", names, count, 0);
      if (sel >= 0) {
        PAPP_LIST it = list;
        for (int i = 0; i < sel && it != NULL; it = it->next)
          ;
        appId = (it != NULL) ? it->id : -1;
      } else {
        appId = find_app_id(list, config->app);
      }
    } else {
      appId = find_app_id(list, config->app);
    }
    free_app_list(list);
  }

  // Home (short press) during the app list backs out to the main menu.
  if (wii_proc_want_main_menu())
    return;

  if (appId < 0 && !haveList)
    appId = get_app_id(client, server, config->app);

  if (appId < 0) {
    char m[256];
    snprintf(m, sizeof(m), "Can't find app %s", config->app);
    show_error(m);
    draw_status("Stream failed");
    wait_for_button();
    return;
  }

  int gamepads = wii_input_num_controllers();
  int gamepad_mask = 0;
  for (int i = 0; i < gamepads; i++)
    gamepad_mask = (gamepad_mask << 1) + 1;

  Font_Clear();
  Font_SetColor(255, 255, 255, 255);
  Font_SetSize(24);
  Font_Print(8, 20, "Starting stream...");
  Font_Draw_TVDRC();

  int ret = gs_start_app(client, server, &config->stream, appId, server->isGfe, config->sops, config->localaudio, gamepad_mask);
  if (ret < 0) {
    if (ret == GS_NOT_SUPPORTED_4K)
      show_error("Server doesn't support 4K");
    else if (ret == GS_NOT_SUPPORTED_MODE) {
      char m[256];
      snprintf(m, sizeof(m), "Server doesn't support %dx%d (%d fps)", config->stream.width, config->stream.height, config->stream.fps);
      show_error(m);
    } else if (ret == GS_NOT_SUPPORTED_SOPS_RESOLUTION)
      show_error("Optimal Playable Settings isn't supported for this resolution");
    else if (ret == GS_ERROR) {
      char m[512];
      snprintf(m, sizeof(m), "Gamestream error:\n%s", gs_get_error_message());
      show_error(m);
    } else {
      char m[256];
      snprintf(m, sizeof(m), "Errorcode starting app: %d", ret);
      show_error(m);
    }
    draw_status("Stream failed");
    wait_for_button();
    return;
  }

  if (config->debug_level > 0) {
    printf("Stream %d x %d, %d fps, %d kbps\n", config->stream.width, config->stream.height, config->stream.fps, config->stream.bitrate);
  }

  if (LiStartConnection(&server->serverInfo, &config->stream, &connection_callbacks, &decoder_callbacks_wii, &audio_callbacks_wii, NULL, 0, config->audio_device, 0) != 0) {
    show_error("Failed to start connection");
    draw_status("Stream failed");
    wait_for_button();
    return;
  }

  // Long-pressing the Wiimote home button stops the stream (see input.c).
  wii_proc_set_home_enabled(1);
  start_input_thread();
  state = STATE_STREAMING;
  while (wii_proc_running() && state == STATE_STREAMING) {
    wii_stream_draw();
  }
  stop_input_thread();
  LiStopConnection();

  if (config->quitappafter) {
    if (config->debug_level > 0)
      printf("Sending app quit request ...\n");
    gs_quit_app(client, server);
  }

  state = STATE_INVALID;
}

static void main_menu(GS_CLIENT client, PSERVER_DATA server, PCONFIGURATION config) {
  int connected = 0;

  while (wii_proc_running()) {
    // A home (short press) from any submenu lands here; clear it so it only
    // unwinds the submenu that set it.
    wii_proc_set_want_main_menu(0);
    if (connected) {
      const char* opts[6];
      int n = 0;
      opts[n++] = "Stream";
      opts[n++] = server->paired ? "Unpair" : "Pair";
      opts[n++] = "Stream settings";
      opts[n++] = "Change IP address";
      opts[n++] = "Disconnect";
      opts[n++] = "Quit";

      char title[128];
      snprintf(title, sizeof(title), "Connected to %s", config->address ? config->address : "?");
      int sel = menu_select(title, opts, n, 0);
      if (sel < 0)
        continue;

      switch (sel) {
        case 0:
          do_stream(client, server, config);
          break;
        case 1:
          if (server->paired)
            do_unpair(client, server);
          else
            do_pair(client, server);
          break;
        case 2:
          settings_menu(config);
          break;
        case 3:
          enter_address(config);
          break;
        case 4:
          connected = 0;
          break;
        case 5:
          wii_proc_stop_running();
          break;
      }
    } else {
      const char* opts[] = { "Connect", "Enter IP address", "Stream settings", "Quit" };
      int sel = menu_select("Moonlight Wii (disconnected)", opts, 4, 0);
      if (sel < 0)
        continue;

      switch (sel) {
        case 0:
          if (config->address == NULL) {
            if (!enter_address(config))
              break;
          }
          if (connect_server(client, server, config) == 0) {
            connected = 1;
            if (autostream)
              do_stream(client, server, config);
          }
          break;
        case 1:
          enter_address(config);
          break;
        case 2:
          settings_menu(config);
          break;
        case 3:
          wii_proc_stop_running();
          break;
      }
    }
  }
}

// Load a specific IOS from a launch argument (e.g. "ios=250"), mirroring
// WiiMC-SSLC. Must run before the network stack is brought up, since
// IOS_ReloadIOS tears down and reinitializes the IOS subsystems.
#ifdef __WII__
static int iosVal = 58;
static bool isReload = false;

static void handle_config_pair(const char* kv) {
  char key[64];
  const char* vs = kv;
  int i = 0;
  while (*vs && *vs != ' ' && *vs != '\t' && *vs != ':' && *vs != '=' && i < (int)sizeof(key) - 1)
    key[i++] = *vs++;
  key[i] = 0;
  while (*vs == ' ' || *vs == '\t' || *vs == ':' || *vs == '=')
    vs++;
  if (strcmp(key, "ios") == 0) {
    isReload = true;
    iosVal = atoi(vs);
  }
}

static void handle_ios_reload(void) {
  if (!isReload)
    return;
  printf("Reloading IOS %d -> %d\n", IOS_GetVersion(), iosVal);
  IOS_ReloadIOS(iosVal);
  printf("IOS reload done, now IOS%d\n", IOS_GetVersion());
}
#endif

int main(int argc, char* argv[]) {
  wii_proc_init();

#ifdef DEBUG
  Debug_Init();
#endif

  // Mode-only video init (WiiMC InitVideo): early, before net/fs/input/font.
  wii_stream_init(640, 480);

  wii_setup_renderstate();

#ifdef __WII__
  for (int i = 1; i < argc; i++)
    handle_config_pair(argv[i]);
  handle_ios_reload();
#endif

  wii_net_init();

  log_clocks();

  wii_fs_init();

  wii_input_init();

  // XFB + GX init (WiiMC InitVideo2): late, after net/fs/input -- the font is
  // brought up next and drawn through the pipeline.
  gx_video_init2();

  Font_Init();

  Font_SetSize(24);
  Font_SetColor(255, 255, 255, 255);
  Font_Print(8, 20, "Reading configuration...");
  Font_Draw_TVDRC();

  CONFIGURATION config;
  config_ensure_wii();
  config_parse(argc, argv, &config);

  Font_SetOverscan(config.osd_overscan);

  // TODO
  config.unsupported = true;
  config.sops = false;

  GS_CLIENT client = create_client(&config);
  if (client == NULL) {
    wait_for_button();
    Font_Deinit();
    wii_stream_fini();
    wii_net_shutdown();
    wii_proc_shutdown();
    return 1;
  }

  if (config.tls_test != NULL)
    wii_tls_test(config.tls_test, config.key_dir);

  SERVER_DATA server;
  main_menu(client, &server, &config);

  gs_destroy(client);

  Font_Deinit();

  wii_stream_fini();

  wii_net_shutdown();

  wii_proc_shutdown();

  return 0;
}
