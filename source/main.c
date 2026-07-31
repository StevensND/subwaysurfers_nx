/* Subway Surfers 3.66.1 Unity/IL2CPP host for Nintendo Switch. */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "asset_pack.h"

#define DATA_ROOT  GAME_HOME
#define LIB_MAIN   "lib/arm64-v8a/libmain.so"
#define LIB_UNITY  "lib/arm64-v8a/libunity.so"
#define LIB_IL2CPP "lib/arm64-v8a/libil2cpp.so"

/* Force FMOD output type 22 (OpenSL ES) instead of Java AudioTrack type 21. */
#define SS_FMOD_SETOUTPUT_SITE 0x831484
#define SS_FMOD_SETOUTPUT_FROM 0x2A1503E1u    /* mov w1, w21 (requested type) */

/* Bypass the two waits driven by Android Choreographer callbacks. */
#define SS_CHOREO_WAIT_SITE    0x71e820     /* FrameTimeTracker ctor wait-loop exit test */
#define SS_CHOREO_WAIT_FROM    0xb50000a8u  /* cbnz x8, 0x71e834 */
#define SS_CHOREO_WAIT_TO      0x14000005u  /* b 0x71e834 (fall through; object fully constructed) */
#define SS_WAITVSYNC_SITE      0x71be2c     /* WaitVSync counter-wait exit test */
#define SS_WAITVSYNC_FROM      0x540000aau  /* b.ge 0x71be40 */
#define SS_WAITVSYNC_TO        0x14000005u  /* b 0x71be40 (don't wait for the frozen vsync counter) */

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
extern void oc_exclude_range(void *base, size_t len);
void  *g_oc_pool_base = NULL;
size_t g_oc_pool_size = 0;
int    g_oc_want = 0;
u64    g_stack_base = 0, g_stack_size = 0;

#define TLS_GUARD_SLOTS 96
#define TLS_GUARD_MAX   128

static Handle g_tls_guard_slots[TLS_GUARD_SLOTS];
static Handle g_tls_guard_sentinels[TLS_GUARD_MAX - TLS_GUARD_SLOTS];
static size_t g_tls_guard_slot_count;
static size_t g_tls_guard_sentinel_count;
static int g_tls_guard_ready;
static Mutex g_tls_guard_lock;
static uint8_t g_tls_guard_stack[0x1000] __attribute__((aligned(0x1000)));

extern Result __real_svcCreateThread(Handle *out, void *entry, void *arg,
                                     void *stack_top, int prio, int cpuid);

static void tls_guard_entry(void *arg) {
  (void)arg;
  svcExitThread();
}

static size_t tls_pages_in_stack(void) {
  if (!g_stack_base || !g_stack_size) return 0;
  const u64 end = g_stack_base + g_stack_size;
  size_t pages = 0;
  for (u64 addr = g_stack_base; addr < end; ) {
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
    u64 next = mi.addr + mi.size;
    if (next <= addr) break;
    if (mi.type == MemType_ThreadLocal) pages += (size_t)(mi.size / 0x1000);
    addr = next < end ? next : end;
  }
  return pages;
}

static void tls_guard_prepare(void) {
  size_t initial_pages = tls_pages_in_stack();
  if (!initial_pages) return;

  while (g_tls_guard_slot_count < TLS_GUARD_SLOTS) {
    size_t before = tls_pages_in_stack();
    Handle handle = INVALID_HANDLE;
    Result rc = __real_svcCreateThread(&handle, (void *)tls_guard_entry, NULL,
                                       g_tls_guard_stack + sizeof g_tls_guard_stack,
                                       0x2C, -2);
    if (R_FAILED(rc)) break;

    size_t after = tls_pages_in_stack();
    if (after > before) {
      if (g_tls_guard_sentinel_count >=
          sizeof g_tls_guard_sentinels / sizeof g_tls_guard_sentinels[0]) {
        svcCloseHandle(handle);
        break;
      }
      g_tls_guard_sentinels[g_tls_guard_sentinel_count++] = handle;
    } else {
      g_tls_guard_slots[g_tls_guard_slot_count++] = handle;
    }
  }

  g_tls_guard_ready = g_tls_guard_slot_count != 0;
}

static void tls_guard_release_slots(void) {
  mutexLock(&g_tls_guard_lock);
  while (g_tls_guard_slot_count)
    svcCloseHandle(g_tls_guard_slots[--g_tls_guard_slot_count]);
  g_tls_guard_ready = 0;
  mutexUnlock(&g_tls_guard_lock);
}

Result __wrap_svcCreateThread(Handle *out, void *entry, void *arg,
                              void *stack_top, int prio, int cpuid) {
  if (!g_tls_guard_ready)
    return __real_svcCreateThread(out, entry, arg, stack_top, prio, cpuid);

  mutexLock(&g_tls_guard_lock);
  if (g_tls_guard_slot_count)
    svcCloseHandle(g_tls_guard_slots[--g_tls_guard_slot_count]);
  Result rc = __real_svcCreateThread(out, entry, arg, stack_top, prio, cpuid);
  mutexUnlock(&g_tls_guard_lock);
  return rc;
}

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Supply TimeManager::Update with a monotonic clock when Android frame timing
 * is unavailable. The hook preserves the original 2022.3 prologue fields. */
static void (*g_unity_update_body)(void *, double) = NULL; /* 0x4410f8 Update body */
static uint64_t g_clk_base_ns = 0;
static void   *g_tm = NULL;
static Mutex   g_clock_lock;
static volatile uint64_t g_last_main_tick_ns = 0;
#define CLOCK_STALL_NS 100000000ULL
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall   = (double)(now - g_clk_base_ns) / 1e9;
  double sref   = *(volatile double *)((char *)tm + 0xe8);
  double newTime = sref + wall;
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;
  g_last_main_tick_ns = nx_now_ns();
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return;
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
/* Keep time advancing while the render thread waits on synchronous scene work. */
static Thread g_clock_thr;
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);
    void *tm = g_tm;
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {
      nx_clock_tick(tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_start_clock_thread(void) {
  Result create_rc = 0, start_rc = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    memset(&g_clock_thr, 0, sizeof g_clock_thr);
    create_rc = threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL,
                             0x8000, 0x2C, -2);
    if (R_SUCCEEDED(create_rc)) {
      start_rc = threadStart(&g_clock_thr);
      if (R_SUCCEEDED(start_rc)) return;
      threadClose(&g_clock_thr);
    } else {
      start_rc = create_rc;
    }
    svcSleepThread(25000000ULL);
  }
  fatal_error("Could not start the engine clock thread (%08x/%08x).",
              create_rc, start_rc);
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  g_unity_update_body = (void (*)(void *, double))(ub + OFF_TimeManager_Update_body);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  if (so_patch_code((void *)(ub + OFF_TimeManager_Update_entry), stub, sizeof stub) != 0)
    fatal_error("Could not patch the Unity clock.");
}

/* Address space reserved for the three Android modules. */
#define SO_REGION_BYTES (192u * 1024 * 1024)

/* Partition the process heap into managed, commit-pool, module and mmap zones. */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;
  const size_t MB = 1024 * 1024;
  int overridden = envHasHeapOverride();

  if (overridden) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
  }

  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  /* Back sparse mmap reservations from free stack-region holes. */
  {
    const size_t managed = OC_MANAGED_BYTES, pool = OC_POOL_BYTES;
    size_t heap_total = 0;
    if (!overridden) {
      size_t oc_heap = managed + pool + so_zone + OC_OVERFLOW_BYTES;
      if (oc_heap + 64 * MB < size && R_SUCCEEDED(svcSetHeapSize(&addr, oc_heap)))
        heap_total = oc_heap;
    } else if (size >= managed + pool + so_zone + 128 * MB) {
      heap_total = size;
    }
    if (heap_total) {
      uintptr_t hs = (uintptr_t)addr;
      uintptr_t heap_end   = hs + heap_total;
      uintptr_t arena_base = ALIGN_MEM(hs + managed + pool + so_zone, MMAP_ARENA_ALIGN);
      size_t overflow = (heap_end > arena_base)
                        ? ((heap_end - arena_base) & ~(size_t)(MMAP_ARENA_ALIGN - 1)) : 0;
      fake_heap_start   = (char *)addr;
      fake_heap_end     = (char *)addr + managed;
      g_oc_pool_base    = (void *)(hs + managed);
      g_oc_pool_size    = pool;
      heap_so_base      = (void *)ALIGN_MEM(hs + managed + pool, 0x1000);
      heap_so_limit     = so_zone;
      g_mmap_arena_base = (void *)arena_base;
      g_mmap_arena_size = overflow;
      g_oc_want         = 1;
      return;
    }
  }

  /* Heap-backed mode for constrained launch environments. */
  if (!overridden) {
    if (R_FAILED(svcSetHeapSize(&addr, size)))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    /* Preserve enough newlib heap for IL2CPP allocations. */
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * ARENA_CAP_PCT) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static int install_setting(const char *name, const void *data, size_t size) {
  char path[768];
  struct stat st;
  snprintf(path, sizeof path, DATA_ROOT "/settings/%s", name);
  if (stat(path, &st) == 0) return 1;
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (fd < 0) return errno == EEXIST;
  size_t done = 0;
  while (done < size) {
    ssize_t put = write(fd, (const char *)data + done, size - done);
    if (put <= 0) break;
    done += (size_t)put;
  }
  int ok = done == size;
  if (ok && fsync(fd) != 0) ok = 0;
  if (close(fd) != 0) ok = 0;
  if (!ok) unlink(path);
  return ok;
}

static void mirror_packaged_settings(void) {
  const char *src_dir = DATA_ROOT "/assets/settings";
  const char *dst_dir = DATA_ROOT "/settings";
  if (mkdir(dst_dir, 0777) < 0 && errno != EEXIST)
    fatal_error("Could not create the settings directory.");

  if (asset_pack_active()) {
    for (size_t i = 0; i < asset_pack_entry_count(); i++) {
      const char *relative = asset_pack_entry_path(i);
      if (strncmp(relative, "settings/", 9) || strchr(relative + 9, '/')) continue;
      void *data = NULL;
      size_t size = 0;
      if (!asset_pack_read_all_relative(relative, &data, &size) ||
          !install_setting(relative + 9, data, size)) {
        free(data);
        fatal_error("Could not install packaged setting %s.", relative + 9);
      }
      free(data);
    }
    return;
  }

  DIR *dir = opendir(src_dir);
  if (!dir) fatal_error("Missing packaged settings directory.");
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

    char src[768], dst[768];
    struct stat src_st, dst_st;
    snprintf(src, sizeof src, "%s/%s", src_dir, entry->d_name);
    snprintf(dst, sizeof dst, "%s/%s", dst_dir, entry->d_name);
    if (stat(src, &src_st) < 0 || !S_ISREG(src_st.st_mode)) continue;

    if (stat(dst, &dst_st) == 0) continue;
    int in = open(src, O_RDONLY);
    void *data = in >= 0 ? malloc((size_t)src_st.st_size ? (size_t)src_st.st_size : 1) : NULL;
    size_t done = 0;
    while (data && done < (size_t)src_st.st_size) {
      ssize_t got = read(in, (char *)data + done, (size_t)src_st.st_size - done);
      if (got <= 0) break;
      done += (size_t)got;
    }
    int ok = data && done == (size_t)src_st.st_size &&
             install_setting(entry->d_name, data, done);
    if (in >= 0) close(in);
    free(data);
    if (!ok) {
      closedir(dir);
      fatal_error("Could not install packaged setting %s.", entry->d_name);
    }
  }
  closedir(dir);
}

static void remove_tree(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) { unlink(path); return; }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child[768];
    struct stat st;
    snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
    else unlink(child);
  }
  closedir(dir);
  rmdir(path);
}

static void create_asset_skeleton(void) {
  static const char *dirs[] = {
    "assets", "assets/aa", "assets/bin", "assets/bin/Data",
    "assets/bin/Data/Managed", "assets/bin/Data/Managed/Metadata",
    "assets/settings", "assets/tower", "assets/tower/client",
    "assets/tower/gamedata",
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    char path[768];
    snprintf(path, sizeof path, DATA_ROOT "/%s", dirs[i]);
    if (mkdir(path, 0777) < 0 && errno != EEXIST)
      fatal_error("Could not prepare the packed asset layout.");
  }
}

static int has_suffix(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcmp(s + a - b, suffix);
}

/* Accept old staged releases once, then keep the normal APK extraction tree. */
static void migrate_legacy_modules(void) {
  const char *names[] = { "libmain.so", "libunity.so", "libil2cpp.so" };
  mkdir(DATA_ROOT "/lib", 0777);
  mkdir(DATA_ROOT "/lib/arm64-v8a", 0777);
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
    char old_path[768], new_path[768];
    struct stat st;
    snprintf(old_path, sizeof old_path, "%s/%s", DATA_ROOT, names[i]);
    snprintf(new_path, sizeof new_path, "%s/lib/arm64-v8a/%s", DATA_ROOT, names[i]);
    if (stat(new_path, &st) == 0) { unlink(old_path); continue; }
    if (stat(old_path, &st) == 0 && rename(old_path, new_path) != 0)
      fatal_error("Could not migrate %s to the extracted APK layout.", names[i]);
  }
}

/* Remove files supplied by Android packaging but unused by the native host. */
static void cleanup_apk_extract(void) {
  static const char *diagnostics[] = {
    "wallet_debug.log", "save_debug.log", "mmap_debug.log",
    "bootstrap_telemetry.csv", "pack_io_telemetry.csv", ".offline_consent_v1",
  };
  static const char *dirs[] = {
    "META-INF", "res", "kotlin", "explorestack", "google", "okhttp3", "org", "src",
    "lib/armeabi-v7a", "assets/ad-viewer", "assets/bm_networks", "assets/dexopt",
  };
  static const char *files[] = {
    "base.apk", "AndroidManifest.xml", "resources.arsc", "DebugProbesKt.bin",
    "androidsupportmultidexversion.txt", "info.txt", "stamp-cert-sha256",
    "age-signals.properties", "billing.properties", "client_analytics.proto",
    "core-common.properties", "firebase-analytics.properties",
    "firebase-annotations.properties", "firebase-encoders-proto.properties",
    "firebase-encoders.properties", "firebase-iid-interop.properties",
    "firebase-iid.properties", "firebase-measurement-connector.properties",
    "messaging_event_extension.proto", "messaging_event.proto",
    "play-services-ads-api.properties", "play-services-ads-identifier.properties",
    "play-services-ads.properties", "play-services-appset.properties",
    "play-services-base.properties", "play-services-basement.properties",
    "play-services-cloud-messaging.properties", "play-services-cronet.properties",
    "play-services-games-v2.properties", "play-services-location.properties",
    "play-services-measurement-api.properties", "play-services-measurement-base.properties",
    "play-services-measurement-impl.properties", "play-services-measurement-sdk-api.properties",
    "play-services-measurement-sdk.properties", "play-services-measurement.properties",
    "play-services-nearby.properties", "play-services-places-placereport.properties",
    "play-services-stats.properties", "play-services-tasks.properties",
    "user-messaging-platform.properties", "version.properties",
    "assets/audience_network.dex", "assets/com.moloco.sdk.xenoss.sdkdevkit.mraid.js",
    "assets/fyb_iframe_endcard_tmpl.html", "assets/fyb_static_endcard_tmpl.html",
    "assets/ia_js_load_monitor.txt", "assets/ia_mraid_bridge.txt",
    "assets/mbridge_download_dialog_view.xml", "assets/mraid-bridge.js",
    "assets/mraid.js", "assets/rv_binddatas.xml",
  };
  for (unsigned i = 0; i < sizeof(diagnostics) / sizeof(*diagnostics); i++) {
    char path[768];
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, diagnostics[i]);
    unlink(path);
  }
  if (access(DATA_ROOT "/AndroidManifest.xml", F_OK) != 0 &&
      access(DATA_ROOT "/classes.dex", F_OK) != 0 &&
      access(DATA_ROOT "/base.apk", F_OK) != 0 &&
      access(DATA_ROOT "/res", F_OK) != 0)
    return;
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, dirs[i]);
    remove_tree(path);
  }
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++) {
    char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    unlink(path);
  }

  DIR *root = opendir(DATA_ROOT);
  if (root) {
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL) {
      const char *n = entry->d_name;
      if (!strncmp(n, "classes", 7) && has_suffix(n, ".dex")) {
        char path[768]; snprintf(path, sizeof path, "%s/%s", DATA_ROOT, n);
        unlink(path);
      }
    }
    closedir(root);
  }

  DIR *libs = opendir(DATA_ROOT "/lib/arm64-v8a");
  if (libs) {
    struct dirent *entry;
    while ((entry = readdir(libs)) != NULL) {
      const char *n = entry->d_name;
      if (!strcmp(n, ".") || !strcmp(n, "..") ||
          !strcmp(n, "libmain.so") || !strcmp(n, "libunity.so") ||
          !strcmp(n, "libil2cpp.so")) continue;
      char path[768];
      snprintf(path, sizeof path, "%s/lib/arm64-v8a/%s", DATA_ROOT, n);
      unlink(path);
    }
    closedir(libs);
  }
}

static int data_file_stat(const char *relative, uint64_t *size) {
  if (asset_pack_active() && !strncmp(relative, "assets/", 7))
    return asset_pack_stat_relative(relative + 7, size, NULL);
  char path[768];
  struct stat st;
  snprintf(path, sizeof path, DATA_ROOT "/%s", relative);
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  if (size) *size = (uint64_t)st.st_size;
  return 1;
}

static int file_contains(const char *relative, const char *needle) {
  void *data = NULL;
  size_t used = 0;
  uint64_t file_size = 0;
  if (!data_file_stat(relative, &file_size) || file_size == 0 || file_size > (16 << 20))
    return 0;
  if (asset_pack_active() && !strncmp(relative, "assets/", 7)) {
    if (!asset_pack_read_all_relative(relative + 7, &data, &used)) return 0;
  } else {
    data = malloc((size_t)file_size);
    if (!data) return 0;
    char path[768];
    snprintf(path, sizeof path, DATA_ROOT "/%s", relative);
    int fd = open(path, O_RDONLY);
    while (fd >= 0 && used < (size_t)file_size) {
      ssize_t got = read(fd, (char *)data + used, (size_t)file_size - used);
      if (got <= 0) break;
      used += (size_t)got;
    }
    if (fd >= 0) close(fd);
  }
  size_t n = strlen(needle);
  int found = 0;
  for (size_t i = 0; !found && i + n <= used; i++)
    found = !memcmp((char *)data + i, needle, n);
  free(data);
  return found;
}

static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/globalgamemanagers",
    "assets/bin/Data/level0",
    "assets/aa/catalog.json",
    "assets/tower/client/manifest.json",
    "assets/tower/gamedata/characters.json",
    "assets/tower/gamedata/external.json",
    "assets/settings/sybo.localization.json",
    "settings/sybo.localization.json",
  };
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    if (!data_file_stat(files[i], NULL))
      fatal_error("Missing data file:\n%s\nCheck your SD card layout (see BUILD.md).", files[i]);
  }
  if (!file_contains("assets/bin/Data/globalgamemanagers", SS_VERSION_NAME) ||
      !file_contains("assets/bin/Data/globalgamemanagers", "2022.3.62f2"))
    fatal_error("The extracted game is not Subway Surfers 3.66.1 / Unity 2022.3.62f2.");
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  resolve_module_imports(mod);
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* Reduce Unity allocator regions from 256 MB to 64 MB. All original words are
 * verified before the 21-site patch is applied. */
static void nx_patch_unity_regions(uintptr_t ub) {
  static const struct { uint32_t off, from, to; } P[] = {
    /* VirtualAllocator block-table index math. */
    {0x49b370, 0xd35cdc33, 0xd35ad433}, {0x49b374, 0xd35cfd15, 0xd35afd15},
    {0x49b404, 0x52a20008, 0x52a08008}, {0x49b740, 0xd35cfc28, 0xd35afc28},
    {0x49b750, 0x92646c28, 0x92667428}, {0x49b758, 0xd35c9c2a, 0xd35a942a},
    {0x49b76c, 0xb25c6feb, 0xb25e77eb}, {0x49b770, 0xd35cdc29, 0xd35ad429}, /* COMPOSITE A */
    {0x49b774, 0xf2a2000b, 0xf2a0800b}, {0x49b7b4, 0xcb0a7108, 0xcb0a6908}, /* COMPOSITE A */
    {0x49b7dc, 0xd35c9c29, 0xd35a9429}, {0x49d4a8, 0xd35c9e89, 0xd35a9689},
    /* Region rounding and DynamicHeap size. */
    {0x499368, 0x12be000a, 0x12bf800a}, {0x499370, 0x92648d36, 0x92669536},
    {0x4948ec, 0x12be0009, 0x12bf8009}, {0x4948f4, 0x92648d36, 0x92669536},
    {0x498ebc, 0xd35cfd29, 0xd35afd29}, {0x498ec0, 0x52a2000a, 0x52a0800a},
    /* TLS and bucket size clamps. */
    {0x495190, 0xd35cfc28, 0xd35afc28}, {0x495194, 0x52a20009, 0x52a08009},
    {0x496bec, 0x52a20009, 0x52a08009},
  };
  const int N = (int)(sizeof P / sizeof P[0]);
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur != P[i].from) fatal_error("Unsupported libunity.so memory layout.");
  }
  for (int i = 0; i < N; i++) {
    if (so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to) != 0)
      fatal_error("Could not patch libunity.so memory layout.");
  }
}

/* Provide the Android application paths that Unity normally gets from Context. */
#define SS_PATHICALL_FROM  0xd10143ffu            /* sub sp, sp, #0x50 (shared by all 3) */
static void *(*g_il2cpp_string_new)(const char *) = NULL;
static void *(*g_il2cpp_array_new)(void *elem_klass, unsigned long len) = NULL;
static void *g_ss_persist_str = NULL, *g_ss_streaming_str = NULL, *g_ss_datapath_str = NULL;
static void *g_ss_userdata_str = NULL;

/* Resolve System.Net.Dns through libnx and return IL2CPP string arrays. */
static int ss_dns_gethostbyname_icall(void *host, void **h_name, void **h_aliases,
                                      void **h_addr_list, int hint, void *method) {
  (void)hint; (void)method;
  if (!host || !g_il2cpp_string_new || !g_il2cpp_array_new) return 0;
  char hb[256];
  int len = *(int *)((char *)host + 0x10);
  const uint16_t *ch = (const uint16_t *)((char *)host + 0x14);
  int i = 0; for (; i < len && i < 255; i++) hb[i] = (char)ch[i];
  hb[i] = 0;
  void *string_klass = *(void **)host;   /* host->klass == System.String */

  struct addrinfo hints; memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = NULL;
  int rc = getaddrinfo(hb, NULL, &hints, &res);
  if (rc != 0 || !res) {
    if (res) freeaddrinfo(res);
    return 0;
  }
  int naddr = 0; struct addrinfo *p;
  for (p = res; p && naddr < 16; p = p->ai_next) if (p->ai_family == AF_INET) naddr++;
  if (naddr == 0) { freeaddrinfo(res); return 0; }

  void *addr_arr = g_il2cpp_array_new(string_klass, (unsigned long)naddr);
  int idx = 0;
  for (p = res; p && idx < naddr; p = p->ai_next) {
    if (p->ai_family != AF_INET) continue;
    char ip[INET_ADDRSTRLEN];
    struct sockaddr_in *sin = (struct sockaddr_in *)p->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
    ((void **)((char *)addr_arr + 0x20))[idx] = g_il2cpp_string_new(ip);
    idx++;
  }
  freeaddrinfo(res);
  *h_name      = g_il2cpp_string_new(hb);
  *h_aliases   = g_il2cpp_array_new(string_klass, 0);
  *h_addr_list = addr_arr;
  return 1;
}
/* NetworkConnectionListener values used by the managed HTTP stack. */
static int ss_net_status_wifi(void) { return 1; }   /* NetworkStatus.ConnectedToWIFI */
static void *ss_persistentdatapath_hook(void) {
  if (!g_ss_persist_str && g_il2cpp_string_new) {
    g_ss_persist_str = g_il2cpp_string_new("/switch/subwaysurfers_nx");
  }
  return g_ss_persist_str;
}
static void *ss_streamingassetspath_hook(void) {
  if (!g_ss_streaming_str && g_il2cpp_string_new) {
    g_ss_streaming_str = g_il2cpp_string_new("/switch/subwaysurfers_nx/assets");
  }
  return g_ss_streaming_str;
}
static void *ss_datapath_hook(void) {
  if (!g_ss_datapath_str && g_il2cpp_string_new) {
    g_ss_datapath_str = g_il2cpp_string_new("/switch/subwaysurfers_nx");
  }
  return g_ss_datapath_str;
}
static void nx_patch_path_icall(uintptr_t ub, uint32_t off, void *hook) {
  volatile uint32_t *site = (volatile uint32_t *)(ub + off);
  if (*site != SS_PATHICALL_FROM) fatal_error("Unsupported libunity.so.");
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)hook & 0xffffffffu),
    (uint32_t)((uintptr_t)hook >> 32),
  };
  if (so_patch_code((void *)site, stub, sizeof stub) != 0)
    fatal_error("Could not patch libunity.so.");
}
/* Give the legacy-profile migrator a valid local directory. */
#define SS_GETANDROIDUSERDATA_RVA 0x225B664u
static void *ss_getandroidUserdata_hook(void *method) {
  (void)method;   /* static il2cpp method: only the trailing MethodInfo* arg, ignored */
  if (!g_ss_userdata_str && g_il2cpp_string_new)
    g_ss_userdata_str = g_il2cpp_string_new("/switch/subwaysurfers_nx");
  return g_ss_userdata_str;
}
static void nx_patch_il2cpp_method(uint32_t rva, void *hook) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  volatile uint32_t *site = (volatile uint32_t *)(ib + rva);
  uint32_t entry = *site;
  /* Guard a dump/binary RVA mismatch: a real AArch64 prologue is never 0 / all-ones. */
  if (entry == 0u || entry == 0xffffffffu) fatal_error("Unsupported libil2cpp.so.");
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)hook & 0xffffffffu),
    (uint32_t)((uintptr_t)hook >> 32),
  };
  if (so_patch_code((void *)site, stub, sizeof stub) != 0)
    fatal_error("Could not patch libil2cpp.so.");
}
/* Redirect one IL2CPP method to another method with the same ABI. */
static void nx_redirect_il2cpp_method(uint32_t from_rva, uint32_t to_rva) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  volatile uint32_t *site = (volatile uint32_t *)(ib + from_rva);
  uintptr_t target = ib + to_rva;
  uint32_t entry = *site;
  if (entry == 0u || entry == 0xffffffffu) fatal_error("Unsupported libil2cpp.so.");
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)(target & 0xffffffffu),
    (uint32_t)(target >> 32),
  };
  if (so_patch_code((void *)site, stub, sizeof stub) != 0)
    fatal_error("Could not patch libil2cpp.so.");
}
/* Patch one verified AArch64 instruction. */
static void nx_patch_word(uint32_t rva, uint32_t expect, uint32_t neww) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  volatile uint32_t *site = (volatile uint32_t *)(ib + rva);
  uint32_t cur = *site;
  if (cur != expect) fatal_error("Unsupported libil2cpp.so.");
  if (so_patch_code((void *)site, &neww, 4) != 0)
    fatal_error("Could not patch libil2cpp.so.");
}

#define SS_BSA_INITIALIZE_RVA    0x3DD7260u
#define SS_BSA_FILE_EXISTS_RVA   0x3DD74C4u
#define SS_BSA_OPEN_READ_RVA     0x3DD7674u
#define SS_BSA_READ_TEXT_RVA     0x3DD7950u
#define SS_BSA_READ_BYTES_RVA    0x3DD7AD8u
#define SS_BSA_GET_FILES_ALL_RVA 0x3DD7E98u
#define SS_FILE_OPEN_READ_RVA    0x39CA450u
#define SS_FILE_READ_TEXT_RVA    0x39CA4B8u
#define SS_FILE_READ_BYTES_RVA   0x39CA970u

static void *(*g_file_open_read)(void *, void *);
static void *(*g_file_read_text)(void *, void *);
static void *(*g_file_read_bytes)(void *, void *);

static int ss_string_utf8(void *str, char *out, size_t size) {
  if (!str || !out || size == 0) return 0;
  int length = *(int *)((char *)str + 0x10);
  const uint16_t *src = (const uint16_t *)((char *)str + 0x14);
  size_t used = 0;
  for (int i = 0; i < length; i++) {
    uint32_t cp = src[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < length &&
        src[i + 1] >= 0xdc00 && src[i + 1] <= 0xdfff) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (src[++i] - 0xdc00);
    }
    size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (used + need >= size) return 0;
    if (need == 1) out[used++] = (char)cp;
    else if (need == 2) {
      out[used++] = (char)(0xc0 | (cp >> 6));
      out[used++] = (char)(0x80 | (cp & 0x3f));
    } else if (need == 3) {
      out[used++] = (char)(0xe0 | (cp >> 12));
      out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      out[used++] = (char)(0x80 | (cp & 0x3f));
    } else {
      out[used++] = (char)(0xf0 | (cp >> 18));
      out[used++] = (char)(0x80 | ((cp >> 12) & 0x3f));
      out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      out[used++] = (char)(0x80 | (cp & 0x3f));
    }
  }
  out[used] = 0;
  return 1;
}

static int ss_asset_relative(void *str, char *out, size_t size) {
  char raw[640];
  if (!ss_string_utf8(str, raw, sizeof raw)) return 0;
  for (char *p = raw; *p; p++) if (*p == '\\') *p = '/';
  const char *p = raw;
  while (*p == '/') p++;
  if (!strncmp(p, "assets/", 7)) p += 7;
  size_t used = 0;
  while (*p) {
    while (*p == '/') p++;
    const char *start = p;
    while (*p && *p != '/') p++;
    size_t n = (size_t)(p - start);
    if (!n || (n == 1 && start[0] == '.')) continue;
    if (n == 2 && start[0] == '.' && start[1] == '.') return 0;
    if (used && used + 1 >= size) return 0;
    if (used) out[used++] = '/';
    if (used + n >= size) return 0;
    memcpy(out + used, start, n);
    used += n;
  }
  out[used] = 0;
  return 1;
}

static void *ss_asset_managed_path(void *path) {
  char rel[640], full[768];
  if (!ss_asset_relative(path, rel, sizeof rel)) return NULL;
  snprintf(full, sizeof full, "/switch/subwaysurfers_nx/assets%s%s",
           rel[0] ? "/" : "", rel);
  return g_il2cpp_string_new(full);
}

static void ss_bsa_initialize(void *data_path, void *streaming_path, void *method) {
  (void)data_path; (void)streaming_path; (void)method;
}

static int ss_bsa_file_exists(void *path, void *method) {
  (void)method;
  char rel[640], full[768];
  struct stat st;
  int found = 0;
  if (ss_asset_relative(path, rel, sizeof rel)) {
    if (asset_pack_active()) found = asset_pack_stat_relative(rel, NULL, NULL);
    else {
      snprintf(full, sizeof full, DATA_ROOT "/assets%s%s", rel[0] ? "/" : "", rel);
      found = stat(full, &st) == 0 && S_ISREG(st.st_mode);
    }
  }
  return found;
}

static void *ss_bsa_open_read(void *path, void *method) {
  (void)method;
  void *full = ss_asset_managed_path(path);
  void *result = full ? g_file_open_read(full, NULL) : NULL;
  return result;
}

static void *ss_bsa_read_text(void *path, void *method) {
  (void)method;
  void *full = ss_asset_managed_path(path);
  void *result = full ? g_file_read_text(full, NULL) : NULL;
  return result;
}

static void *ss_bsa_read_bytes(void *path, void *method) {
  (void)method;
  void *full = ss_asset_managed_path(path);
  void *result = full ? g_file_read_bytes(full, NULL) : NULL;
  return result;
}

static int ss_glob(const char *pattern, const char *name) {
  const char *star = NULL, *retry = NULL;
  while (*name) {
    if (*pattern == '?' || *pattern == *name) { pattern++; name++; continue; }
    if (*pattern == '*') { star = pattern++; retry = name; continue; }
    if (star) { pattern = star + 1; name = ++retry; continue; }
    return 0;
  }
  while (*pattern == '*') pattern++;
  return *pattern == 0;
}

typedef struct { char **items; size_t count, capacity; } SsPathList;

static int ss_path_add(SsPathList *list, const char *path) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 32;
    char **items = realloc(list->items, capacity * sizeof(*items));
    if (!items) return 0;
    list->items = items;
    list->capacity = capacity;
  }
  list->items[list->count] = strdup(path);
  if (!list->items[list->count]) return 0;
  list->count++;
  return 1;
}

static int ss_collect_files(const char *rel, const char *pattern, int recursive,
                            SsPathList *list) {
  char dir_path[768];
  snprintf(dir_path, sizeof dir_path, DATA_ROOT "/assets%s%s", rel[0] ? "/" : "", rel);
  DIR *dir = opendir(dir_path);
  if (!dir) return 1;
  struct dirent *entry;
  int ok = 1;
  while (ok && (entry = readdir(dir)) != NULL) {
    const char *name = entry->d_name;
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    char child_rel[640], child_path[768];
    struct stat st;
    if (snprintf(child_rel, sizeof child_rel, "%s%s%s", rel, rel[0] ? "/" : "", name)
        >= (int)sizeof child_rel) continue;
    if (snprintf(child_path, sizeof child_path, DATA_ROOT "/assets/%s", child_rel)
        >= (int)sizeof child_path) continue;
    if (stat(child_path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (recursive) ok = ss_collect_files(child_rel, pattern, recursive, list);
    } else if (S_ISREG(st.st_mode) && ss_glob(pattern, name)) {
      ok = ss_path_add(list, child_rel);
    }
  }
  closedir(dir);
  return ok;
}

static int ss_collect_packed_files(const char *rel, const char *pattern, int recursive,
                                   SsPathList *list) {
  size_t prefix = strlen(rel);
  for (size_t i = 0; i < asset_pack_entry_count(); i++) {
    const char *path = asset_pack_entry_path(i);
    const char *tail = path;
    if (prefix) {
      if (strncmp(path, rel, prefix) || path[prefix] != '/') continue;
      tail = path + prefix + 1;
    }
    if (!recursive && strchr(tail, '/')) continue;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (ss_glob(pattern, name) && !ss_path_add(list, path)) return 0;
  }
  return 1;
}

static int ss_path_compare(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void *ss_bsa_get_files_impl(void *path, void *pattern_obj, int recursive) {
  char rel[640], pattern[256] = "*";
  if (!ss_asset_relative(path, rel, sizeof rel)) goto failed;
  if (pattern_obj && !ss_string_utf8(pattern_obj, pattern, sizeof pattern)) goto failed;
  if (!pattern[0]) strcpy(pattern, "*");

  SsPathList list = {0};
  int collected = asset_pack_active()
                    ? ss_collect_packed_files(rel, pattern, recursive, &list)
                    : ss_collect_files(rel, pattern, recursive, &list);
  if (!collected) {
    for (size_t i = 0; i < list.count; i++) free(list.items[i]);
    free(list.items);
    goto failed;
  }
  if (list.count > 1)
    qsort(list.items, list.count, sizeof(*list.items), ss_path_compare);
  void *string_class = *(void **)path;
  void *array = g_il2cpp_array_new(string_class, (unsigned long)list.count);
  if (array) {
    void **data = (void **)((char *)array + 0x20);
    for (size_t i = 0; i < list.count; i++) data[i] = g_il2cpp_string_new(list.items[i]);
  }
  for (size_t i = 0; i < list.count; i++) free(list.items[i]);
  free(list.items);
  return array;

failed:
  return NULL;
}

static void *ss_bsa_get_files_all(void *path, void *pattern, int option, void *method) {
  (void)method;
  return ss_bsa_get_files_impl(path, pattern, option == 1);
}

static void nx_install_loose_streaming_assets(void) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  g_file_open_read = (void *(*)(void *, void *))(ib + SS_FILE_OPEN_READ_RVA);
  g_file_read_text = (void *(*)(void *, void *))(ib + SS_FILE_READ_TEXT_RVA);
  g_file_read_bytes = (void *(*)(void *, void *))(ib + SS_FILE_READ_BYTES_RVA);
  nx_patch_il2cpp_method(SS_BSA_INITIALIZE_RVA, (void *)&ss_bsa_initialize);
  nx_patch_il2cpp_method(SS_BSA_FILE_EXISTS_RVA, (void *)&ss_bsa_file_exists);
  nx_patch_il2cpp_method(SS_BSA_OPEN_READ_RVA, (void *)&ss_bsa_open_read);
  nx_patch_il2cpp_method(SS_BSA_READ_TEXT_RVA, (void *)&ss_bsa_read_text);
  nx_patch_il2cpp_method(SS_BSA_READ_BYTES_RVA, (void *)&ss_bsa_read_bytes);
  nx_patch_il2cpp_method(SS_BSA_GET_FILES_ALL_RVA, (void *)&ss_bsa_get_files_all);
}

static uint32_t nx_arm64_branch(uintptr_t from, uintptr_t to, int link) {
  int64_t delta = (int64_t)to - (int64_t)from;
  if ((delta & 3) || delta < -0x8000000LL || delta > 0x7fffffcLL)
    fatal_error("A libil2cpp.so branch target is out of range.");
  return (link ? 0x94000000u : 0x14000000u) |
         ((uint32_t)(delta >> 2) & 0x03ffffffu);
}

#define SS_WALLET_SET_SILENT_RVA   0x3D0F3BCu
#define SS_WALLET_GET_RVA          0x3D0ED40u
#define SS_WALLET_INIT_SET_RET_RVA 0x23DF044u
#define SS_WALLET_SET_TRAMP_RVA    0x2274B10u

typedef void (*ss_wallet_set_fn)(void *, int, int, int64_t, void *);
typedef int (*ss_wallet_get_fn)(void *, int, void *);

static ss_wallet_set_fn g_wallet_set_orig;
static ss_wallet_get_fn g_wallet_get;

static void ss_wallet_set_hook(void *wallet, int type, int value,
                               int64_t expiration, void *method) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  int before = g_wallet_get(wallet, type, NULL);
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  if (caller == ib + SS_WALLET_INIT_SET_RET_RVA && before > 0) return;
  g_wallet_set_orig(wallet, type, value, expiration, method);
}

static void *nx_install_method_hook(uint32_t rva, uint32_t expected,
                                    uint32_t trampoline_rva, void *hook) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  uintptr_t entry = ib + rva;
  uintptr_t trampoline = ib + trampoline_rva;
  if (*(volatile uint32_t *)entry != expected)
    fatal_error("Unsupported libil2cpp.so wallet patch site.");
  uint32_t code[8];
  for (size_t i = 0; i < 4; i++) code[i] = ((volatile uint32_t *)entry)[i];
  code[4] = 0x58000050u;
  code[5] = 0xD61F0200u;
  code[6] = (uint32_t)((entry + 16) & 0xffffffffu);
  code[7] = (uint32_t)((entry + 16) >> 32);
  if (so_patch_code((void *)trampoline, code, sizeof code) != 0)
    fatal_error("Could not install the wallet patch trampoline.");
  uint32_t stub[4] = {
    0x58000050u, 0xD61F0200u,
    (uint32_t)((uintptr_t)hook & 0xffffffffu),
    (uint32_t)((uintptr_t)hook >> 32),
  };
  if (so_patch_code((void *)entry, stub, sizeof stub) != 0)
    fatal_error("Could not install the wallet patch.");
  return (void *)trampoline;
}

static void nx_install_wallet_guard(void) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  g_wallet_get = (ss_wallet_get_fn)(ib + SS_WALLET_GET_RVA);
  g_wallet_set_orig = (ss_wallet_set_fn)nx_install_method_hook(
    SS_WALLET_SET_SILENT_RVA, 0xD10103FFu, SS_WALLET_SET_TRAMP_RVA, (void *)&ss_wallet_set_hook);
}

/* Reject invalid class pointers before IsAssignableFrom dereferences them. */
#define SS_ISASSIGN_ENTRY_RVA   0x1E376A4u
#define SS_ISASSIGN_ENTRY_WORD  0xa9bd5ffeu   /* stp x30,x23,[sp,#-48]! -- verified before patch */
#define SS_ISASSIGN_CONT_RVA    0x1E376B4u
#define SS_ISASSIGN_ADRP_TGT    0x49C3000u
#define SS_IL2CPP_X_LO_RVA      0x1BB75C4u
#define SS_IL2CPP_X_HI_RVA      0x46AD800u
static uintptr_t g_il2cpp_x_lo = 0, g_il2cpp_x_hi = 0;
static int (*g_isassign_orig)(void *klass, void *oklass) = NULL;   /* -> exec trampoline */
static void (*g_extract_exc_orig)(void *, void **, void **, void *) = NULL;  /* -> trampoline */
static void *g_ss_empty_exc_str = NULL;
/* The replaced DNS body provides executable space for the two trampolines. */
#define SS_ISASSIGN_TRAMP_RVA   0x1E1B288u
#define SS_DNS_IMPL_RVA         0x1E1B278u
#define SS_DNS_IMPL_PATCHED     0x58000050u    /* ldr x16,#8 -- our stub's first word */
static int ss_isassignable_guard(void *klass, void *oklass) {
  uintptr_t a = (uintptr_t)klass, b = (uintptr_t)oklass;
  if (!a || (a & 7u) || (a >= g_il2cpp_x_lo && a < g_il2cpp_x_hi)) return 0;
  if (!b || (b & 7u) || (b >= g_il2cpp_x_lo && b < g_il2cpp_x_hi)) return 0;
  return g_isassign_orig(klass, oklass);
}
static void nx_install_isassignable_guard(void) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  uintptr_t entry = ib + SS_ISASSIGN_ENTRY_RVA;
  uint32_t w0 = *(volatile uint32_t *)entry;
  if (w0 != SS_ISASSIGN_ENTRY_WORD) fatal_error("Unsupported libil2cpp.so type guard.");
  uintptr_t dns = ib + SS_DNS_IMPL_RVA;
  if (*(volatile uint32_t *)dns != SS_DNS_IMPL_PATCHED)
    fatal_error("Could not reserve the libil2cpp.so guard trampoline.");
  uintptr_t tramp = ib + SS_ISASSIGN_TRAMP_RVA;
  g_il2cpp_x_lo = ib + SS_IL2CPP_X_LO_RVA;
  g_il2cpp_x_hi = ib + SS_IL2CPP_X_HI_RVA;
  uintptr_t V    = ib + SS_ISASSIGN_ADRP_TGT;
  uintptr_t cont = ib + SS_ISASSIGN_CONT_RVA;
  uint32_t tr[11]; int n = 0;
  tr[n++] = 0xa9bd5ffeu;                                          /* stp x30,x23,[sp,#-48]! */
  tr[n++] = 0xa90157f6u;                                          /* stp x22,x21,[sp,#16]   */
  tr[n++] = 0xa9024ff4u;                                          /* stp x20,x19,[sp,#32]   */
  tr[n++] = 0xd2800016u | (uint32_t)(( V        & 0xffffu) << 5); /* movz x22,#h0           */
  tr[n++] = 0xf2a00016u | (uint32_t)(((V >> 16) & 0xffffu) << 5); /* movk x22,#h1,lsl16      */
  tr[n++] = 0xf2c00016u | (uint32_t)(((V >> 32) & 0xffffu) << 5); /* movk x22,#h2,lsl32      */
  tr[n++] = 0xf2e00016u | (uint32_t)(((V >> 48) & 0xffffu) << 5); /* movk x22,#h3,lsl48      */
  tr[n++] = 0x58000050u;                                          /* ldr x16,#8 */
  tr[n++] = 0xd61f0200u;                                          /* br  x16    */
  tr[n++] = (uint32_t)(cont & 0xffffffffu);
  tr[n++] = (uint32_t)(cont >> 32);
  if (so_patch_code((void *)tramp, tr, (size_t)n * 4) != 0)
    fatal_error("Could not install the libil2cpp.so guard trampoline.");
  g_isassign_orig = (int (*)(void *, void *))tramp;
  uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ss_isassignable_guard & 0xffffffffu),
    (uint32_t)((uintptr_t)&ss_isassignable_guard >> 32),
  };
  if (so_patch_code((void *)entry, stub, sizeof stub) != 0)
    fatal_error("Could not install the libil2cpp.so type guard.");
}
/* Return empty text when the exception formatter receives an invalid object. */
#define SS_EXTRACTEXC_RVA        0x440A7D8u
#define SS_EXTRACTEXC_WORD       0xa9ba7bfdu   /* stp x29,x30,[sp,#-96]! -- verified before patch */
#define SS_EXTRACTEXC_CONT_RVA   0x440A7E8u
#define SS_EXTRACTEXC_TRAMP_RVA  0x1E1B2B4u
/* Native caller may not null-check the out-slots, so hand back an interned string, not null. */
static void ss_extract_exc_guard(void *exceptiono, void **out_msg, void **out_trace, void *method) {
  uintptr_t k = exceptiono ? *(uintptr_t *)exceptiono : 0;
  if (!exceptiono || (k & 7u) || (k >= g_il2cpp_x_lo && k < g_il2cpp_x_hi)) {
    if (!g_ss_empty_exc_str && g_il2cpp_string_new)
      g_ss_empty_exc_str = g_il2cpp_string_new("(offline exception suppressed)");
    if (out_msg)   *out_msg   = g_ss_empty_exc_str;
    if (out_trace) *out_trace = g_ss_empty_exc_str;
    return;
  }
  g_extract_exc_orig(exceptiono, out_msg, out_trace, method);
}
static void nx_install_exception_formatter_guard(void) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  uintptr_t entry = ib + SS_EXTRACTEXC_RVA;
  uint32_t w0 = *(volatile uint32_t *)entry;
  if (w0 != SS_EXTRACTEXC_WORD) fatal_error("Unsupported libil2cpp.so exception guard.");
  uintptr_t dns = ib + SS_DNS_IMPL_RVA;
  if (*(volatile uint32_t *)dns != SS_DNS_IMPL_PATCHED)
    fatal_error("Could not reserve the exception guard trampoline.");
  g_il2cpp_x_lo = ib + SS_IL2CPP_X_LO_RVA;   /* idempotent with the IsAssignableFrom install */
  g_il2cpp_x_hi = ib + SS_IL2CPP_X_HI_RVA;
  uintptr_t tramp = ib + SS_EXTRACTEXC_TRAMP_RVA;
  uintptr_t cont  = ib + SS_EXTRACTEXC_CONT_RVA;
  uint32_t tr[8]; int n = 0;
  tr[n++] = 0xa9ba7bfdu;   /* stp x29,x30,[sp,#-96]! */
  tr[n++] = 0xa9016ffcu;   /* stp x28,x27,[sp,#16]   */
  tr[n++] = 0xa90267fau;   /* stp x26,x25,[sp,#32]   */
  tr[n++] = 0xa9035ff8u;   /* stp x24,x23,[sp,#48]   */
  tr[n++] = 0x58000050u;   /* ldr x16,#8 */
  tr[n++] = 0xd61f0200u;   /* br  x16    */
  tr[n++] = (uint32_t)(cont & 0xffffffffu);
  tr[n++] = (uint32_t)(cont >> 32);
  if (so_patch_code((void *)tramp, tr, (size_t)n * 4) != 0)
    fatal_error("Could not install the exception guard trampoline.");
  g_extract_exc_orig = (void (*)(void *, void **, void **, void *))tramp;
  uint32_t stub[4] = {
    0x58000050u, 0xd61f0200u,
    (uint32_t)((uintptr_t)&ss_extract_exc_guard & 0xffffffffu),
    (uint32_t)((uintptr_t)&ss_extract_exc_guard >> 32),
  };
  if (so_patch_code((void *)entry, stub, sizeof stub) != 0)
    fatal_error("Could not install the exception guard.");
}
/* Treat unavailable CDN-only Addressables as completed preloads. */
#define SS_PRELOAD_ONREQUESTFAILED_RVA 0x1F394CCu
#define SS_PRELOAD_ONREQUESTDONE_RVA   0x1F395FCu

/* Replace required online-auth stages with the game's completed no-op task. */
#define SS_BOOT_AUTH_REFRESH_DOEXEC   0x22735CCu
#define SS_BOOT_AUTH_WITHSTORE_DOEXEC 0x2273FB8u
#define SS_BOOT_AUTH_INIT_DOEXEC      0x2274AC8u
#define SS_BOOT_NULL_DOEXEC           0x2278E3Cu
/* Suppress optional service failures in the bootstrap sequence. */
#define SS_BOOT_ONSTAGEFAILED         0x3BEA248u
static void *ss_bootstrap_stagefailed_hook(void) { return NULL; }
/* Accept the local guest identity without a server verification round trip. */
#define SS_AUTH_STATE_LOADED          0x3BDF268u
#define SS_AUTH_STATE_VERIFIED        0x3BDF308u
#define SS_PROFILE_MERGE_BEGIN_RVA    0x23F21A4u
#define SS_PROFILE_MERGE_DONE_RVA     0x23F2264u
/* Let managed HTTP requests reach the socket bridge. */
#define SS_NET_HASINTERNET            0x3B829A4u
#define SS_AD_BOOT_CONSENT_RVA        0x2268750u
#define SS_SET_CONSENT_VALUE_RVA      0x232BDACu
#define SS_GLOBAL_CONSENT_RVA         0x3C03DB4u
#define SS_GDPR_CONSENT_GIVEN_RVA     0x232B760u
static int ss_return_true(void) { return 1; }
static int ss_return_false(void) { return 0; }
#define SS_ANDROID_VIB_INIT_RVA        0x22C7B98u
#define SS_ANDROID_VIB_STANDARD_RVA    0x22C8438u
#define SS_ANDROID_VIB_HAPTIC_RVA      0x22C8454u
#define SS_ANDROID_VIB_STANDARD_OK_RVA 0x22C852Cu
#define SS_ANDROID_VIB_HAPTIC_OK_RVA   0x22C8548u

static void ss_vibration_init_hook(void *self, void *logger, void *method) {
  (void)self; (void)logger; (void)method;
}
static void ss_vibration_standard_hook(void *self, int length_ms, void *method) {
  (void)self; (void)method;
  android_native_vibration_standard(length_ms);
}
static void ss_vibration_haptic_hook(void *self, int style, void *method) {
  (void)self; (void)method;
  android_native_vibration_haptic(style);
}
static int ss_vibration_standard_supported_hook(void *self, void *method) {
  (void)self; (void)method;
  return 1;
}
static int ss_vibration_haptic_supported_hook(void *self, int style, void *method) {
  (void)self; (void)style; (void)method;
  return 1;
}

#define SS_IAP_TYPE_SITE_RVA       0x236B34Cu
#define SS_IAP_TYPE_SITE_WORD      0xF9456908u
#define SS_IAP_CTOR_CALL_RVA       0x236B364u
#define SS_IAP_CTOR_CALL_WORD      0x97FFDDECu
#define SS_DEBUG_IAP_CTOR_RVA      0x2367FA8u
#define SS_IAP_TYPE_TRAMP_RVA      0x1E1B2D4u
#define SS_DEBUG_IAP_INIT_FLAG_RVA 0x2369A80u
#define SS_DEBUG_IAP_INIT_DELAY_RVA 0x23699E8u
#define SS_SHOP_IAP_GATE_RVA       0x2144D94u
#define SS_SHOP_IAP_BEGIN_RVA      0x2144F08u
#define SS_SHOP_IAP_DISPATCH_RVA   0x2454D84u
#define SS_SHOP_BUILD_SUCCESS_RVA  0x2450004u
#define SS_SHOP_IAP_SUCCESS_RVA    0x2454E00u

static void *g_debug_iap_class;
static void *(*g_il2cpp_domain_get)(void);
static const void **(*g_il2cpp_domain_get_assemblies)(void *, size_t *);
static const void *(*g_il2cpp_assembly_get_image)(const void *);
static void *(*g_il2cpp_class_from_name)(const void *, const char *, const char *);

static void **ss_debug_iap_type_slot(void) {
  if (!g_debug_iap_class) {
    void *domain = g_il2cpp_domain_get();
    size_t count = 0;
    const void **assemblies = domain ? g_il2cpp_domain_get_assemblies(domain, &count) : NULL;
    for (size_t i = 0; assemblies && i < count && !g_debug_iap_class; i++) {
      const void *image = g_il2cpp_assembly_get_image(assemblies[i]);
      if (image)
        g_debug_iap_class = g_il2cpp_class_from_name(
          image, "SYBO.Subway.Meta", "DebugIAPPurchasesImplementation");
    }
    if (!g_debug_iap_class)
      fatal_error("The offline store implementation is unavailable.");
  }
  return &g_debug_iap_class;
}

static void nx_install_offline_iap(void) {
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  g_il2cpp_domain_get = (void *(*)(void))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get");
  g_il2cpp_domain_get_assemblies = (const void **(*)(void *, size_t *))
    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_domain_get_assemblies");
  g_il2cpp_assembly_get_image = (const void *(*)(const void *))
    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_assembly_get_image");
  g_il2cpp_class_from_name = (void *(*)(const void *, const char *, const char *))
    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_class_from_name");
  if (!g_il2cpp_domain_get || !g_il2cpp_domain_get_assemblies ||
      !g_il2cpp_assembly_get_image || !g_il2cpp_class_from_name)
    fatal_error("Required IL2CPP reflection exports are missing.");

  uintptr_t tramp = ib + SS_IAP_TYPE_TRAMP_RVA;
  uintptr_t resume = ib + SS_IAP_TYPE_SITE_RVA + 4;
  uintptr_t helper = (uintptr_t)&ss_debug_iap_type_slot;
  uint32_t code[12] = {
    0xA9BF7BFDu,
    0x580000F0u,
    0xD63F0200u,
    0xA8C17BFDu,
    0xAA0003E8u,
    0x580000B0u,
    0xD61F0200u,
    0xD503201Fu,
    (uint32_t)(helper & 0xffffffffu), (uint32_t)(helper >> 32),
    (uint32_t)(resume & 0xffffffffu), (uint32_t)(resume >> 32),
  };
  if (so_patch_code((void *)tramp, code, sizeof code) != 0)
    fatal_error("Could not install the offline store trampoline.");

  volatile uint32_t *type_site = (volatile uint32_t *)(ib + SS_IAP_TYPE_SITE_RVA);
  if (*type_site != SS_IAP_TYPE_SITE_WORD)
    fatal_error("Unsupported libil2cpp.so store implementation site.");
  uint32_t branch = nx_arm64_branch((uintptr_t)type_site, tramp, 0);
  if (so_patch_code((void *)type_site, &branch, sizeof branch) != 0)
    fatal_error("Could not select the offline store implementation.");

  volatile uint32_t *ctor_site = (volatile uint32_t *)(ib + SS_IAP_CTOR_CALL_RVA);
  if (*ctor_site != SS_IAP_CTOR_CALL_WORD)
    fatal_error("Unsupported libil2cpp.so store constructor site.");
  branch = nx_arm64_branch((uintptr_t)ctor_site, ib + SS_DEBUG_IAP_CTOR_RVA, 1);
  if (so_patch_code((void *)ctor_site, &branch, sizeof branch) != 0)
    fatal_error("Could not initialize the offline store implementation.");

  nx_patch_word(SS_DEBUG_IAP_INIT_FLAG_RVA,  0x39404109u, 0x2A1F03E9u);
  nx_patch_word(SS_DEBUG_IAP_INIT_DELAY_RVA, 0xB9401915u, 0x2A1F03F5u);

  /* Keep player-initiated IAPs on the local reward path. */
  nx_patch_word(
    SS_SHOP_IAP_GATE_RVA,
    0xF9401A80u,
    nx_arm64_branch(ib + SS_SHOP_IAP_GATE_RVA, ib + SS_SHOP_IAP_BEGIN_RVA, 0));

  {
    uintptr_t site = ib + SS_SHOP_IAP_DISPATCH_RVA;
    const uint32_t expected[4] = {
      0xF9401A62u, 0xAA1403E0u, 0x97FFED11u, 0xB40006C0u,
    };
    for (size_t i = 0; i < 4; i++) {
      if (((volatile uint32_t *)site)[i] != expected[i])
        fatal_error("Unsupported libil2cpp.so store purchase site.");
    }
    uint32_t code[4] = {
      0xF9401260u,
      nx_arm64_branch(site + 4, ib + SS_SHOP_BUILD_SUCCESS_RVA, 1),
      0xAA0003F5u,
      nx_arm64_branch(site + 12, ib + SS_SHOP_IAP_SUCCESS_RVA, 0),
    };
    if (so_patch_code((void *)site, code, sizeof code) != 0)
      fatal_error("Could not install the offline store purchase path.");
  }
}

/* Hide account providers that depend on unavailable Android/iOS SDKs. */
#define SS_REQ_FACEBOOK_ALLOWED_RVA      0x2421DA8u
#define SS_REQ_HAS_SOCIAL_PROVIDERS_RVA  0x2423954u
#define SS_SOCIAL_PROVIDER_ALLOWED_RVA   0x2479CACu
static void nx_install_persistentdatapath_hook(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
  g_il2cpp_string_new = (void *(*)(const char *))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_string_new");
  if (!g_il2cpp_string_new) fatal_error("il2cpp_string_new is unavailable.");
  g_il2cpp_array_new = (void *(*)(void *, unsigned long))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_array_new");
  if (!g_il2cpp_array_new) fatal_error("il2cpp_array_new is unavailable.");
  nx_patch_path_icall(ub, 0x3cc280, (void *)&ss_persistentdatapath_hook);
  nx_patch_path_icall(ub, 0x3cc1e8, (void *)&ss_streamingassetspath_hook);
  nx_patch_path_icall(ub, 0x3cc150, (void *)&ss_datapath_hook);
  nx_install_loose_streaming_assets();
  nx_patch_il2cpp_method(SS_GETANDROIDUSERDATA_RVA, (void *)&ss_getandroidUserdata_hook);
  mkdir(GAME_HOME "/Save", 0777);

  /* Install the local guest identity required by offline startup. */
  {
    mkdir(GAME_HOME "/profile", 0777);
    static const char kIdentity[] =
      "{\"user\":{\"id\":\"5d3f9e42-cf2c-82aa-1452-97ab1b480459\",\"name\":\"Player\","
      "\"picture\":\"\",\"links\":[]},"
      "\"refreshToken\":{\"token\":\"nx-offline-refresh-token\"},"
      "\"identityToken\":{\"token\":"
      "\"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJndWVzdCIsImV4cCI6NDEwMjQ0NDgwMH0.c2lnbmF0dXJl\","
      "\"expiresAt\":\"2099-12-31T23:59:59Z\"}}";
    int fd = open(GAME_HOME "/profile/identity.json", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) fatal_error("Could not create the offline identity.");
    ssize_t written = write(fd, kIdentity, sizeof(kIdentity) - 1);
    close(fd);
    if (written != (ssize_t)(sizeof(kIdentity) - 1))
      fatal_error("Could not write the offline identity.");
  }

  nx_redirect_il2cpp_method(SS_PRELOAD_ONREQUESTFAILED_RVA, SS_PRELOAD_ONREQUESTDONE_RVA);
  nx_redirect_il2cpp_method(SS_BOOT_AUTH_REFRESH_DOEXEC,   SS_BOOT_NULL_DOEXEC);
  nx_redirect_il2cpp_method(SS_BOOT_AUTH_WITHSTORE_DOEXEC, SS_BOOT_NULL_DOEXEC);
  nx_redirect_il2cpp_method(SS_BOOT_AUTH_INIT_DOEXEC,      SS_BOOT_NULL_DOEXEC);
  nx_install_wallet_guard();

  nx_patch_il2cpp_method(SS_ANDROID_VIB_INIT_RVA, (void *)&ss_vibration_init_hook);
  nx_patch_il2cpp_method(SS_ANDROID_VIB_STANDARD_RVA, (void *)&ss_vibration_standard_hook);
  nx_patch_il2cpp_method(SS_ANDROID_VIB_HAPTIC_RVA, (void *)&ss_vibration_haptic_hook);
  nx_patch_il2cpp_method(SS_ANDROID_VIB_STANDARD_OK_RVA,
                        (void *)&ss_vibration_standard_supported_hook);
  nx_patch_il2cpp_method(SS_ANDROID_VIB_HAPTIC_OK_RVA,
                        (void *)&ss_vibration_haptic_supported_hook);

  nx_patch_il2cpp_method(SS_REQ_FACEBOOK_ALLOWED_RVA, (void *)&ss_return_false);
  nx_patch_il2cpp_method(SS_REQ_HAS_SOCIAL_PROVIDERS_RVA, (void *)&ss_return_false);
  nx_patch_il2cpp_method(SS_SOCIAL_PROVIDER_ALLOWED_RVA, (void *)&ss_return_false);

  nx_patch_il2cpp_method(SS_BOOT_ONSTAGEFAILED, (void *)&ss_bootstrap_stagefailed_hook);
  nx_redirect_il2cpp_method(SS_AUTH_STATE_LOADED, SS_AUTH_STATE_VERIFIED);
  nx_patch_word(
    SS_PROFILE_MERGE_BEGIN_RVA,
    0xB4000BD4u,
    nx_arm64_branch(ib + SS_PROFILE_MERGE_BEGIN_RVA,
                    ib + SS_PROFILE_MERGE_DONE_RVA, 0));
  nx_patch_il2cpp_method(SS_NET_HASINTERNET, (void *)&ss_return_true);

  /* Android advertising services are unavailable on Switch. */
  nx_patch_word(SS_AD_BOOT_CONSENT_RVA, 0x12000008u, 0x2A1F03E8u);
  nx_patch_word(SS_SET_CONSENT_VALUE_RVA, 0x12000294u, 0x2A1F03F4u);
  nx_patch_il2cpp_method(SS_GLOBAL_CONSENT_RVA, (void *)&ss_return_false);
  nx_patch_il2cpp_method(SS_GDPR_CONSENT_GIVEN_RVA, (void *)&ss_return_false);

  /* Patch the DNS implementation, not its packed four-byte veneer. */
  nx_patch_il2cpp_method(SS_DNS_IMPL_RVA, (void *)&ss_dns_gethostbyname_icall);
  nx_patch_il2cpp_method(0x3C61144u, (void *)&ss_return_true);
  nx_patch_il2cpp_method(0x3C60F78u, (void *)&ss_net_status_wifi);
  nx_install_isassignable_guard();
  nx_install_exception_formatter_guard();
  nx_install_offline_iap();

  /* Select bundled Tower game data instead of the remote archive reader. */
  nx_patch_word(0x22C49D0u, 0x52800064u, 0x52800024u);
  nx_patch_word(0x3DBE8B4u, 0xB9401FE8u, 0x52800028u);
  nx_patch_word(0x3DBEA28u, 0xB9401FE9u, 0x52800029u);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  int optimized_assets = 0;
  {
    extern int g_net_on;
    g_net_on = R_SUCCEEDED(socketInitializeDefault()) ? 1 : 0;
  }
  startup_status_begin("Preparing game data");

  /* Unity and IL2CPP use paths relative to the game directory. */
  if (chdir(DATA_ROOT) != 0) fatal_error("Could not enter %s.", DATA_ROOT);
  migrate_legacy_modules();
  {
    struct stat loose;
    if (stat(DATA_ROOT "/assets/bin/Data/globalgamemanagers", &loose) != 0 ||
        !S_ISREG(loose.st_mode))
      asset_pack_open_existing(DATA_ROOT);
  }

  /* Load language selection and rewrite stale configurations. */
  {
    const char *cfg = DATA_ROOT "/" CONFIG_NAME;
    int rc = read_config(cfg);
    if (rc != 0) write_config(cfg);
  }

  /* Materialize packaged settings for existing SD installations. */
  mirror_packaged_settings();
  startup_status_update("Validating game data");
  check_data();
  startup_status_update("Removing unused Android files");
  cleanup_apk_extract();
  if (!asset_pack_active()) {
    startup_status_update("Optimizing game assets (first boot)");
    if (!asset_pack_build(DATA_ROOT "/assets", DATA_ROOT))
      fatal_error("Could not optimize the extracted assets.\n%s\nThe original files were kept.",
                  asset_pack_error());
    optimized_assets = 1;
  }
  {
    struct stat st;
    if (stat(DATA_ROOT "/assets/bin/Data/globalgamemanagers", &st) == 0 &&
        S_ISREG(st.st_mode)) {
      startup_status_update("Removing unpacked asset files");
      remove_tree(DATA_ROOT "/assets");
      create_asset_skeleton();
    }
  }
  if (optimized_assets)
    startup_status_complete("Game data is ready.\n\n  Launch Subway Surfers again to play.");
  startup_status_update("Starting the game");

  /* Re-extract IL2CPP resources because writable file mappings are not persisted. */
  unlink(DATA_ROOT "/il2cpp/unity.ver");
  unlink(DATA_ROOT "/il2cpp/Metadata/global-metadata.dat");
  unlink(DATA_ROOT "/il2cpp/Resources/mscorlib.dll-resources.dat");

  check_syscalls();

  /* Back large mmap reservations with reserved stack-region holes. */
  if (g_oc_want == 1 && g_oc_pool_base) {
    svcGetInfo(&g_stack_base, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&g_stack_size, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
    tls_guard_prepare();
  }
  nx_start_clock_thread();
  {
    uintptr_t ps = (uintptr_t)g_oc_pool_base, pe = ps + g_oc_pool_size;
    int pool_in_stack = g_stack_base && g_oc_pool_base &&
                        !(pe <= (uintptr_t)g_stack_base || ps >= (uintptr_t)(g_stack_base + g_stack_size));
    if (g_oc_want == 1 && g_oc_pool_base && g_stack_base &&
        g_stack_size >= (u64)OC_MIN_STACK_MB * 1024 * 1024 && !pool_in_stack) {
      uintptr_t rbase = ((uintptr_t)g_stack_base + (MMAP_ARENA_ALIGN - 1)) & ~(uintptr_t)(MMAP_ARENA_ALIGN - 1);
      size_t rsize = ((size_t)g_stack_size - (rbase - (uintptr_t)g_stack_base)) & ~(size_t)(MMAP_ARENA_ALIGN - 1);
      if (oc_arena_init((void *)rbase, rsize, g_oc_pool_base, g_oc_pool_size)) {
        struct { u64 base; u64 size; } holes[24];
        int nh = 0;
        for (u64 a = rbase, end = rbase + rsize; a < end && nh < 24; ) {
          MemoryInfo mi; u32 pi;
          if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
          u64 me = mi.addr + mi.size;
          if (me <= a) break;
          if (mi.type == MemType_Unmapped) {
            u64 hb = mi.addr < rbase ? rbase : mi.addr;
            u64 he = me > end ? end : me;
            if (he > hb) { holes[nh].base = hb; holes[nh].size = he - hb; nh++; }
          }
          a = me;
        }
        int big = 0;
        for (int i = 1; i < nh; i++) if (holes[i].size > holes[big].size) big = i;
        const size_t leave = (size_t)OC_STACK_LEAVE_MB * 1024 * 1024;
        for (int i = 0; i < nh; i++) {
          u64 hb = holes[i].base;
          size_t hsz = (size_t)holes[i].size;
          size_t reserve_len = hsz;
          if (i == big && hsz > leave + MMAP_ARENA_ALIGN) {
            reserve_len = (hsz - leave) & ~(MMAP_ARENA_ALIGN - 1);
            oc_exclude_range((void *)(hb + reserve_len), hsz - reserve_len);
          }
          virtmemLock();
          VirtmemReservation *rv = virtmemAddReservation((void *)hb, reserve_len);
          virtmemUnlock();
          if (!rv) oc_exclude_range((void *)hb, reserve_len);
        }
        g_oc_want = 2;
      }
    }
  }
  if (g_oc_want == 2) {
    tls_guard_release_slots();
  }

  /* Match the surface to the physical display. */
  if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1920; screen_height = 1080; }
  else                                                         { screen_width = 1280; screen_height = 720;  }

  startup_status_end();
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());

  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);

  /* Route FMOD through the OpenSL ES shim instead of Java AudioTrack. */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *site = (volatile uint32_t *)(ub + SS_FMOD_SETOUTPUT_SITE);
    if (*site == SS_FMOD_SETOUTPUT_FROM) {
      uint32_t req_opensl = 0x528002C1u;
      if (so_patch_code((void *)site, &req_opensl, sizeof req_opensl) != 0)
        fatal_error("Could not patch the Unity audio output.");
    } else {
      fatal_error("Unsupported libunity.so at FMOD patch site.");
    }
  }

  /* Apply allocator patches before any module constructors run. */
  nx_patch_unity_regions((uintptr_t)unity_mod.load_virtbase);

  nx_install_persistentdatapath_hook();

  /* Main thread runs init_array + the engine lifecycle; give it its own stable
   * bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);

  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();

  /* Resolve the UnityPlayer JNI entry points. */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Initialize libunity's internal JNI manager. */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    Unity_JNI_OnLoad(fake_vm, NULL);
  }

  /* Register the JavaVM through IL2CPP's exported Android entry point. */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Il2Cpp_JNI_OnLoad = (fn_jnionload)
      so_try_find_addr_rx(&il2cpp_mod, "JNI_OnLoad");
    if (!Il2Cpp_JNI_OnLoad || Il2Cpp_JNI_OnLoad(fake_vm, NULL) != 0x00010006)
      fatal_error("Could not initialize IL2CPP JNI support.");
  }

  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);

  /* Enter the resumed and focused lifecycle states before rendering. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);

  /* Configure IL2CPP and frame pacing before the first render. */
  {
    /* The Android collector depends on POSIX signals unavailable on Switch. */
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (!il2cpp_gc_set_mode || !il2cpp_gc_disable)
      fatal_error("Required IL2CPP GC exports are missing.");
    il2cpp_gc_set_mode(1);
    il2cpp_gc_disable();

    nx_install_time_fix();

    /* Bypass Android Choreographer waits. */
    {
      uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
      static const struct { uint32_t off, from, to; } CH[] = {
        { SS_CHOREO_WAIT_SITE, SS_CHOREO_WAIT_FROM, SS_CHOREO_WAIT_TO },
        { SS_WAITVSYNC_SITE,   SS_WAITVSYNC_FROM,   SS_WAITVSYNC_TO   },
      };
      for (unsigned i = 0; i < sizeof(CH)/sizeof(CH[0]); i++) {
        volatile uint32_t *site = (volatile uint32_t *)(ub + CH[i].off);
        if (*site != CH[i].from) fatal_error("Unsupported libunity.so frame pacing.");
        uint32_t to = CH[i].to;
        if (so_patch_code((void *)site, &to, sizeof to) != 0)
          fatal_error("Could not patch Unity frame pacing.");
      }
    }
  }
  while (appletMainLoop() && !jni_quit_requested) {
    android_native_update_mode();
    android_native_vibration_update();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
  }

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  android_native_vibration_shutdown();
  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
