/* Supplemental Unity and IL2CPP imports. */
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <EGL/egl.h>
#include <zlib.h>
#include <switch.h>
#include <unistd.h>
#include "imports.h"
#include "libc_shim.h"
#include "android_native_unity.h"
#include "asset_pack.h"

/* Bionic-compatible character flags. */
char z_ctype[384];
__attribute__((constructor)) static void z_ctype_init(void){
  for(int c=0;c<256;c++){
    unsigned char f=0;
    if(isupper(c))  f|=0x01;
    if(islower(c))  f|=0x02;
    if(isdigit(c))  f|=0x04;
    if(isspace(c))  f|=0x08;
    if(ispunct(c))  f|=0x10;
    if(iscntrl(c))  f|=0x20;
    if(isxdigit(c)) f|=0x40;
    if(c==' ')      f|=0x80;
    z_ctype[c]=(char)f;
  }
}

/* Unavailable optional APIs. */
static long z_stub0(void){ return 0; }
static int z_vprintf_noop(const char *fmt, va_list va){ (void)fmt; (void)va; return 0; }

#define MEDIA_KEY(symbol, value) static const char *z_##symbol = value
MEDIA_KEY(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_FORMAT, "color-format");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_RANGE, "color-range");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_STANDARD, "color-standard");
MEDIA_KEY(AMEDIAFORMAT_KEY_DURATION, "durationUs");
MEDIA_KEY(AMEDIAFORMAT_KEY_ENCODER_DELAY, "encoder-delay");
MEDIA_KEY(AMEDIAFORMAT_KEY_FRAME_RATE, "frame-rate");
MEDIA_KEY(AMEDIAFORMAT_KEY_HEIGHT, "height");
MEDIA_KEY(AMEDIAFORMAT_KEY_LANGUAGE, "language");
MEDIA_KEY(AMEDIAFORMAT_KEY_MIME, "mime");
MEDIA_KEY(AMEDIAFORMAT_KEY_ROTATION, "rotation-degrees");
MEDIA_KEY(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate");
MEDIA_KEY(AMEDIAFORMAT_KEY_SLICE_HEIGHT, "slice-height");
MEDIA_KEY(AMEDIAFORMAT_KEY_STRIDE, "stride");
MEDIA_KEY(AMEDIAFORMAT_KEY_WIDTH, "width");
#undef MEDIA_KEY

static long z_prctl(int option, unsigned long a2, unsigned long a3,
                    unsigned long a4, unsigned long a5){
  (void)option; (void)a2; (void)a3; (void)a4; (void)a5;
  return 0;
}
static int z_pthread_setname_np(void *thread, const char *name){
  (void)thread; (void)name;
  return 0;
}

/* Report the current thread's mapped stack to IL2CPP. */
int z_pthread_attr_getstack(const void *attr, void **stackaddr, size_t *stacksize){
  (void)attr;
  uintptr_t sp; __asm__ volatile("mov %0, sp" : "=r"(sp));
  void *base; size_t sz;
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)sp)) && mi.size){
    base = (void *)(uintptr_t)mi.addr;
    sz   = (size_t)mi.size;
  } else {
    base = (void *)(sp & ~0xFFFFFull);
    sz   = 0x100000;
  }
  if (stackaddr) *stackaddr = base;
  if (stacksize) *stacksize = sz;
  return 0;
}
int z_pthread_getattr_np(void *thread, void *attr){ (void)thread; (void)attr; return 0; }

int z_getpagesize(void){ return 0x1000; }
int z_pthread_equal(unsigned long a, unsigned long b){ return a==b; }
int z_gettid(void){ return 1; }
int z_dup(int fd){
  if (asset_pack_fd_is(fd)) return asset_pack_dup_fd(fd);
  /* File maps are copied eagerly, so reusing the descriptor is safe. */
  int n = dup(fd);
  if (n < 0) n = fd;
  return n;
}
/* JNI device properties may be null. */
int z_strcasecmp(const char *a, const char *b){
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcasecmp(a, b);
}
char *z_basename(const char *path){
  if (!path || !*path) return (char *)".";
  const char *s = strrchr(path, '/');
  return (char *)(s ? s + 1 : path);
}
/* Locale objects are ignored by newlib's character classifiers. */
int z_isdigit_l (int c, void *l){ (void)l; return isdigit(c); }
int z_islower_l (int c, void *l){ (void)l; return islower(c); }
int z_isupper_l (int c, void *l){ (void)l; return isupper(c); }
int z_isxdigit_l(int c, void *l){ (void)l; return isxdigit(c); }
int z_tolower_l (int c, void *l){ (void)l; return tolower(c); }
int z_toupper_l (int c, void *l){ (void)l; return toupper(c); }
void*z_memrchr(const void*s,int c,unsigned long n){ const unsigned char*p=(const unsigned char*)s+n; while(n--){ if(*--p==(unsigned char)c) return (void*)p; } return 0; }

DynLibFunction unity_dynlib_functions[] = {
  { "ALooper_acquire", (uintptr_t)&ALooper_acquire },
  { "ALooper_forThread", (uintptr_t)&ALooper_forThread },
  { "ALooper_release", (uintptr_t)&ALooper_release },
  { "ALooper_wake", (uintptr_t)&ALooper_wake },
  { "ANativeWindow_acquire", (uintptr_t)&ANativeWindow_acquire },
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release },
#define OPTIONAL_MEDIA_FN(symbol) { #symbol, (uintptr_t)&z_stub0 }
  OPTIONAL_MEDIA_FN(AHardwareBuffer_acquire),
  OPTIONAL_MEDIA_FN(AHardwareBuffer_describe),
  OPTIONAL_MEDIA_FN(AHardwareBuffer_release),
  OPTIONAL_MEDIA_FN(AImage_delete),
  OPTIONAL_MEDIA_FN(AImage_deleteAsync),
  OPTIONAL_MEDIA_FN(AImage_getHardwareBuffer),
  OPTIONAL_MEDIA_FN(AImage_getTimestamp),
  OPTIONAL_MEDIA_FN(AImage_getWidth),
  OPTIONAL_MEDIA_FN(AImageReader_acquireLatestImage),
  OPTIONAL_MEDIA_FN(AImageReader_delete),
  OPTIONAL_MEDIA_FN(AImageReader_getWindow),
  OPTIONAL_MEDIA_FN(AImageReader_newWithUsage),
  OPTIONAL_MEDIA_FN(AImageReader_setBufferRemovedListener),
  OPTIONAL_MEDIA_FN(AImageReader_setImageListener),
  OPTIONAL_MEDIA_FN(AMediaCodec_configure),
  OPTIONAL_MEDIA_FN(AMediaCodec_createDecoderByType),
  OPTIONAL_MEDIA_FN(AMediaCodec_delete),
  OPTIONAL_MEDIA_FN(AMediaCodec_dequeueInputBuffer),
  OPTIONAL_MEDIA_FN(AMediaCodec_dequeueOutputBuffer),
  OPTIONAL_MEDIA_FN(AMediaCodec_flush),
  OPTIONAL_MEDIA_FN(AMediaCodec_getInputBuffer),
  OPTIONAL_MEDIA_FN(AMediaCodec_getOutputBuffer),
  OPTIONAL_MEDIA_FN(AMediaCodec_getOutputFormat),
  OPTIONAL_MEDIA_FN(AMediaCodec_queueInputBuffer),
  OPTIONAL_MEDIA_FN(AMediaCodec_releaseOutputBuffer),
  OPTIONAL_MEDIA_FN(AMediaCodec_setOutputSurface),
  OPTIONAL_MEDIA_FN(AMediaCodec_start),
  OPTIONAL_MEDIA_FN(AMediaCodec_stop),
  OPTIONAL_MEDIA_FN(AMediaDataSource_delete),
  OPTIONAL_MEDIA_FN(AMediaDataSource_new),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setClose),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setGetSize),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setReadAt),
  OPTIONAL_MEDIA_FN(AMediaDataSource_setUserdata),
  OPTIONAL_MEDIA_FN(AMediaExtractor_advance),
  OPTIONAL_MEDIA_FN(AMediaExtractor_delete),
  OPTIONAL_MEDIA_FN(AMediaExtractor_getSampleTime),
  OPTIONAL_MEDIA_FN(AMediaExtractor_getSampleTrackIndex),
  OPTIONAL_MEDIA_FN(AMediaExtractor_getTrackCount),
  OPTIONAL_MEDIA_FN(AMediaExtractor_getTrackFormat),
  OPTIONAL_MEDIA_FN(AMediaExtractor_new),
  OPTIONAL_MEDIA_FN(AMediaExtractor_readSampleData),
  OPTIONAL_MEDIA_FN(AMediaExtractor_seekTo),
  OPTIONAL_MEDIA_FN(AMediaExtractor_selectTrack),
  OPTIONAL_MEDIA_FN(AMediaExtractor_setDataSource),
  OPTIONAL_MEDIA_FN(AMediaExtractor_setDataSourceCustom),
  OPTIONAL_MEDIA_FN(AMediaExtractor_setDataSourceFd),
  OPTIONAL_MEDIA_FN(AMediaFormat_delete),
  OPTIONAL_MEDIA_FN(AMediaFormat_getFloat),
  OPTIONAL_MEDIA_FN(AMediaFormat_getInt32),
  OPTIONAL_MEDIA_FN(AMediaFormat_getInt64),
  OPTIONAL_MEDIA_FN(AMediaFormat_getString),
  OPTIONAL_MEDIA_FN(AMediaFormat_setInt32),
  OPTIONAL_MEDIA_FN(ANativeWindow_toSurface),
#undef OPTIONAL_MEDIA_FN
#define MEDIA_KEY_ENTRY(symbol) { #symbol, (uintptr_t)&z_##symbol }
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_CHANNEL_COUNT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_COLOR_FORMAT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_COLOR_RANGE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_COLOR_STANDARD),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_DURATION),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_ENCODER_DELAY),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_FRAME_RATE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_HEIGHT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_LANGUAGE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_MIME),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_ROTATION),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_SAMPLE_RATE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_SLICE_HEIGHT),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_STRIDE),
  MEDIA_KEY_ENTRY(AMEDIAFORMAT_KEY_WIDTH),
#undef MEDIA_KEY_ENTRY
  { "ASensorEventQueue_hasEvents", (uintptr_t)&ASensorEventQueue_hasEvents },
  { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue },
  { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue },
  { "ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor },
  { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance },
  { "ASensorManager_getSensorList", (uintptr_t)&ASensorManager_getSensorList },
  { "ASensor_getMinDelay", (uintptr_t)&ASensor_getMinDelay },
  { "ASensor_getName", (uintptr_t)&ASensor_getName },
  { "ASensor_getResolution", (uintptr_t)&ASensor_getResolution },
  { "ASensor_getType", (uintptr_t)&ASensor_getType },
  { "ASensor_getVendor", (uintptr_t)&ASensor_getVendor },
  { "_ZTH15gDeferredAction", (uintptr_t)&z_stub0 },
  { "__system_property_find", (uintptr_t)&z_stub0 },
  { "__system_property_read", (uintptr_t)&z_stub0 },
  { "_ctype_", (uintptr_t)&z_ctype[0] },
  { "acos", (uintptr_t)&acos },
  { "asin", (uintptr_t)&asin },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atanf", (uintptr_t)&atanf },
  { "atol", (uintptr_t)&atol },
  { "basename", (uintptr_t)&z_basename },
  { "bsearch", (uintptr_t)&bsearch },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "clearerr", (uintptr_t)&clearerr },
  { "clock", (uintptr_t)&clock },
  { "clock_getres", (uintptr_t)&z_stub0 },
  { "cos", (uintptr_t)&cos },
  { "difftime", (uintptr_t)&difftime },
  { "div", (uintptr_t)&div },
  { "dladdr", (uintptr_t)&z_stub0 },
  { "dup", (uintptr_t)&z_dup },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  { "eglQueryString", (uintptr_t)&eglQueryString },
  { "eglSurfaceAttrib", (uintptr_t)&eglSurfaceAttrib },
  { "eglSwapInterval", (uintptr_t)&eglSwapInterval },
  { "exit", (uintptr_t)&exit },
  { "exp", (uintptr_t)&exp },
  { "exp2f", (uintptr_t)&exp2f },
  { "fdopen", (uintptr_t)&fdopen_fake },
  { "flock", (uintptr_t)&z_stub0 },
  { "fmod", (uintptr_t)&fmod },
  { "fnmatch", (uintptr_t)&z_stub0 },
  { "fscanf", (uintptr_t)&fscanf },
  { "futimens", (uintptr_t)&z_stub0 },
  { "gethostbyaddr", (uintptr_t)&z_stub0 },
  { "gethostbyname", (uintptr_t)&z_stub0 },
  { "getpagesize", (uintptr_t)&z_getpagesize },
  { "getpriority", (uintptr_t)&z_stub0 },
  { "getpwuid_r", (uintptr_t)&z_stub0 },
  { "gettid", (uintptr_t)&z_gettid },
  { "hypot", (uintptr_t)&hypot },
  { "inet_addr", (uintptr_t)&z_stub0 },
  { "inet_ntop", (uintptr_t)&z_stub0 },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "isdigit_l", (uintptr_t)&z_isdigit_l },
  { "islower_l", (uintptr_t)&z_islower_l },
  { "isupper_l", (uintptr_t)&z_isupper_l },
  { "isxdigit_l", (uintptr_t)&z_isxdigit_l },
  { "ldexp", (uintptr_t)&ldexp },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "lldiv", (uintptr_t)&lldiv },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "log2", (uintptr_t)&log2 },
  { "log2f", (uintptr_t)&log2f },
  { "logb", (uintptr_t)&logb },
  { "lrand48", (uintptr_t)&z_stub0 },
  { "lseek64", (uintptr_t)&z_lseek },
  { "madvise", (uintptr_t)&z_stub0 },
  { "memrchr", (uintptr_t)&z_memrchr },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "prctl", (uintptr_t)&z_prctl },
  { "pthread_atfork", (uintptr_t)&z_stub0 },
  { "pthread_attr_getstack", (uintptr_t)&z_pthread_attr_getstack },
  { "pthread_condattr_destroy", (uintptr_t)&z_stub0 },
  { "pthread_condattr_init", (uintptr_t)&z_stub0 },
  { "pthread_condattr_setclock", (uintptr_t)&z_stub0 },
  { "pthread_equal", (uintptr_t)&z_pthread_equal },
  { "pthread_getattr_np", (uintptr_t)&z_pthread_getattr_np },
  { "pthread_rwlock_init", (uintptr_t)&z_stub0 },
  { "pthread_setname_np", (uintptr_t)&z_pthread_setname_np },
  { "ptrace", (uintptr_t)&z_stub0 },
  { "raise", (uintptr_t)&raise },
  { "recvmsg", (uintptr_t)&z_stub0 },
  { "scalbn", (uintptr_t)&scalbn },
  { "sched_getaffinity", (uintptr_t)&z_stub0 },
  { "sched_setaffinity", (uintptr_t)&z_stub0 },
  { "sem_getvalue", (uintptr_t)&z_stub0 },
  { "sendmsg", (uintptr_t)&z_stub0 },
  { "setbuf", (uintptr_t)&setbuf },
  { "setenv", (uintptr_t)&setenv },
  { "setpriority", (uintptr_t)&z_stub0 },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "sigaltstack", (uintptr_t)&z_stub0 },
  { "sigdelset", (uintptr_t)&z_stub0 },
  { "sigfillset", (uintptr_t)&z_stub0 },
  { "sigsuspend", (uintptr_t)&z_stub0 },
  { "sin", (uintptr_t)&sin },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "srand48", (uintptr_t)&z_stub0 },
  { "strcasecmp", (uintptr_t)&z_strcasecmp },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strftime", (uintptr_t)&strftime },
  { "strlcpy", (uintptr_t)&strlcpy },
  { "strnlen", (uintptr_t)&strnlen },
  { "strspn", (uintptr_t)&strspn },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "tan", (uintptr_t)&tan },
  { "tolower_l", (uintptr_t)&z_tolower_l },
  { "toupper_l", (uintptr_t)&z_toupper_l },
  { "towlower", (uintptr_t)&towlower },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "utimes", (uintptr_t)&z_stub0 },
  { "vprintf", (uintptr_t)&z_vprintf_noop },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
};
int unity_dynlib_numfunctions = (int)(sizeof(unity_dynlib_functions)/sizeof(unity_dynlib_functions[0]));
