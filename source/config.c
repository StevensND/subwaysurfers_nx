/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

Config config;
static int config_needs_rewrite = 0;

int screen_width = 1280;
int screen_height = 720;

static void parse_var(const char *name, const char *value) {
  if (!strcmp(name, "touchscreen") || !strcmp(name, "controller_cursor") ||
      !strcmp(name, "show_fps") || !strcmp(name, "widescreen") ||
      !strcmp(name, "screen_width") || !strcmp(name, "screen_height") ||
      !strcmp(name, "portrait")) {
    config_needs_rewrite = 1;
    return;
  }
  if (!strcmp(name, "language")) config.language = atoi(value);
}

int read_config(const char *file) {
  char line[1024];

  memset(&config, 0, sizeof(Config));
  config_needs_rewrite = 0;
  config.language = LANG_EN;

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  while (fgets(line, sizeof(line), f)) {
    char name[64], value[64];
    if (sscanf(line, " %63s %63s", name, value) == 2 && name[0] != '#')
      parse_var(name, value);
  }

  fclose(f);

  return config_needs_rewrite ? 1 : 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f, "language %d\n", config.language);

  fclose(f);

  return 0;
}
