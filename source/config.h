/* Subway Surfers 3.66.1 Switch wrapper configuration. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)1920 * 1024 * 1024)
#define OC_MANAGED_BYTES    ((size_t)1280 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 768 * 1024 * 1024)
#define OC_OVERFLOW_BYTES   ((size_t) 512 * 1024 * 1024)
#define OC_STACK_LEAVE_MB   256u
#define OC_MIN_STACK_MB     1024u
#define ARENA_CAP_PCT       68u

#define SS_PACKAGE        "com.kiloo.subwaysurf"
#define SS_VERSION_CODE   93371
#define SS_VERSION_NAME   "3.66.1"

#define CONFIG_NAME "config.txt"
#define GAME_HOME   "sdmc:/switch/subwaysurfers_nx"

extern int screen_width;
extern int screen_height;

// Language. 0 = follow the Switch system language.
#define LANG_AUTO 0
#define LANG_JA   1
#define LANG_EN   2

typedef struct {
  int language;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
