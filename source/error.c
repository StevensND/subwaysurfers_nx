/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"

static int status_active;

void startup_status_update(const char *message) {
  if (!status_active) return;
  printf("\x1b[2J\x1b[H\n\n  Subway Surfers\n\n  %s\n\n  Please wait...", message);
  consoleUpdate(NULL);
}

void startup_status_begin(const char *message) {
  if (!status_active) {
    consoleInit(NULL);
    status_active = 1;
  }
  startup_status_update(message);
}

void startup_status_end(void) {
  if (!status_active) return;
  consoleExit(NULL);
  status_active = 0;
}

void fatal_error(const char *fmt, ...) {
  char message[1024];
  va_list list;
  va_start(list, fmt);
  vsnprintf(message, sizeof message, fmt, list);
  va_end(list);

  FILE *log = fopen("sdmc:/switch/subwaysurfers_nx/fatal.txt", "w");
  if (log) { fprintf(log, "%s\n", message); fclose(log); }

  if (status_active) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("\x1b[2J\x1b[H\n\n  Subway Surfers could not start\n\n  %s\n\n  Press A to exit.", message);
    consoleUpdate(NULL);
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
    }
    startup_status_end();
  } else {
    ErrorApplicationConfig error;
    if (R_SUCCEEDED(errorApplicationCreate(&error, message, message)))
      errorApplicationShow(&error);
  }

  exit(1);
}
