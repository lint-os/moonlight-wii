/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
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

#include <stdio.h>
#include <stdarg.h>
#include <signal.h>

// Negotiated encryption state (set during the RTSP handshake before
// connectionStarted fires). SS_ENC_* bits cover Sunshine; GFE audio is tracked
// by AudioEncryptionEnabled and remote-input is always encrypted.
#include "Limelight-internal.h"

#ifdef __WII__
#include "wii/wii.h"
#endif

#ifdef HAVE_SDL
#include <SDL.h>
#endif

ConnListenerRumble rumble_handler = NULL;
ConnListenerRumbleTriggers rumble_triggers_handler = NULL;
ConnListenerSetMotionEventState set_motion_event_state_handler = NULL;
ConnListenerSetControllerLED set_controller_led_handler = NULL;

static void connection_terminated(int errorCode) {
  switch (errorCode) {
  case ML_ERROR_GRACEFUL_TERMINATION:
    printf("Connection has been terminated gracefully.\n");
#ifdef __WII__
    sprintf(message_buffer, "Connection has been terminated gracefully.\n");
    is_error = 0;
#endif
    break;
  case ML_ERROR_NO_VIDEO_TRAFFIC:
    printf("No video received from host. Check the host PC's firewall and port forwarding rules.\n");
#ifdef __WII__
    sprintf(message_buffer, "No video received from host.\n Check the host PC's firewall and port forwarding rules.\n");
    is_error = 1;
#endif
    break;
  case ML_ERROR_NO_VIDEO_FRAME:
    printf("Your network connection isn't performing well. Reduce your video bitrate setting or try a faster connection.\n");
#ifdef __WII__
    sprintf(message_buffer, "Your network connection isn't performing well.\n Reduce your video bitrate setting or try a faster connection.\n");
    is_error = 1;
#endif
    break;
  case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
    printf("The connection was unexpectedly terminated by the host due to a video capture error. Make sure no DRM-protected content is playing on the host.\n");
#ifdef __WII__
    sprintf(message_buffer, "The connection was unexpectedly terminated by the host due to a video capture error.\n Make sure no DRM-protected content is playing on the host.\n");
    is_error = 1;
#endif
    break;
  case ML_ERROR_PROTECTED_CONTENT:
    printf("The connection was terminated by the host due to DRM-protected content. Close any DRM-protected content on the host and try again.\n");
#ifdef __WII__
    sprintf(message_buffer, "The connection was terminated by the host due to DRM-protected content.\n Close any DRM-protected content on the host and try again.\n");
    is_error = 1;
#endif
    break;
  default:
    printf("Connection terminated with error: %d\n", errorCode);
#ifdef __WII__
    sprintf(message_buffer, "Connection terminated with error: %d\n", errorCode);
    is_error = 1;
#endif
    break;
  }

#ifndef __WII__
  #ifdef HAVE_SDL
      SDL_Event event;
      event.type = SDL_QUIT;
      SDL_PushEvent(&event);
  #endif
#else
  state = STATE_STOP_STREAM;
#endif
}

static void connection_log_message(const char* format, ...) {
  va_list arglist;
  va_start(arglist, format);
  vprintf(format, arglist);
  va_end(arglist);
}

static void rumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor) {
  if (rumble_handler)
    rumble_handler(controllerNumber, lowFreqMotor, highFreqMotor);
}

static void rumble_triggers(unsigned short controllerNumber, unsigned short leftTrigger, unsigned short rightTrigger) {
  if (rumble_triggers_handler)
    rumble_triggers_handler(controllerNumber, leftTrigger, rightTrigger);
}

static void set_motion_event_state(unsigned short controllerNumber, unsigned char motionType, unsigned short reportRateHz) {
  if (set_motion_event_state_handler)
    set_motion_event_state_handler(controllerNumber, motionType, reportRateHz);
}

static void set_controller_led(unsigned short controllerNumber, unsigned char r, unsigned char g, unsigned char b) {
  if (set_controller_led_handler)
    set_controller_led_handler(controllerNumber, r, g, b);
}

static void connection_status_update(int status) {
  switch (status) {
    case CONN_STATUS_OKAY:
      printf("Connection is okay\n");
      break;
    case CONN_STATUS_POOR:
      printf("Connection is poor\n");
      break;
  }
}

// Fires once the connection is fully established (after the RTSP handshake),
// so the negotiated encryption state is final. Log what is actually encrypted.
static void connection_started(void) {
  int video = (EncryptionFeaturesEnabled & SS_ENC_VIDEO) != 0;
  int audio = AudioEncryptionEnabled || (EncryptionFeaturesEnabled & SS_ENC_AUDIO) != 0;
  int control = (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) != 0;
  printf("Encryption: video=%s audio=%s control=%s remote-input=always\n",
         video ? "yes" : "no", audio ? "yes" : "no", control ? "yes" : "no");
}

CONNECTION_LISTENER_CALLBACKS connection_callbacks = {
  .stageStarting = NULL,
  .stageComplete = NULL,
  .stageFailed = NULL,
  .connectionStarted = connection_started,
  .connectionTerminated = connection_terminated,
  .logMessage = connection_log_message,
  .rumble = rumble,
  .connectionStatusUpdate = connection_status_update,
  .setHdrMode = NULL,
  .rumbleTriggers = rumble_triggers,
  .setMotionEventState = set_motion_event_state,
  .setControllerLED = set_controller_led,
};
