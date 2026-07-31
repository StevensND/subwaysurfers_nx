/* Android NDK window, looper, sensor, and input shims used by libunity. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

static u32 g_w = 1280, g_h = 720;

void android_native_update_mode(void){
  if (appletGetOperationMode() == AppletOperationMode_Console) { g_w = 1920; g_h = 1080; }
  else                                                         { g_w = 1280; g_h = 720;  }
}
static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  nwindowSetDimensions(w, bw, bh);
  nwindowSetTransform(w, 0u);
}

static ANativeWindow *native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_w, g_h);
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return native_window();
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  if (width > 0 && height > 0) nx_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  return 0;
}

/* Unity uses ALooper as a per-thread wait/wake primitive. */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 0); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Sensors are unavailable. */
void *ASensorManager_getInstance(void){ static int x; return &x; }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* Handheld touch is injected directly. In docked mode, the left stick controls
 * a virtual cursor and A taps it. */
#include "unity_input.h"

static PadState g_pad;
static HidTouchScreenState g_touch;
static int   g_prev_touch = 0;        /* pointers down last frame */
static float g_cursor_x = 640, g_cursor_y = 360;
static float g_last_tx = 640, g_last_ty = 360;
static int   g_prev_a = 0;

#define VIBRATION_HANDLE_CAP 8
static HidVibrationDeviceHandle g_vibration_handles[VIBRATION_HANDLE_CAP];
static int g_vibration_count;
static int g_vibration_active;
static u64 g_vibration_deadline;
static Mutex g_vibration_lock;

static void vibration_add(HidNpadIdType id, HidNpadStyleTag style, int count) {
  if (g_vibration_count + count > VIBRATION_HANDLE_CAP) return;
  if (R_SUCCEEDED(hidInitializeVibrationDevices(
        &g_vibration_handles[g_vibration_count], count, id, style)))
    g_vibration_count += count;
}

static void vibration_send(const HidVibrationValue *value) {
  bool permitted = false;
  if (R_FAILED(hidIsVibrationPermitted(&permitted)) || !permitted) return;
  for (int i = 0; i < g_vibration_count; i++) {
    bool mounted = false;
    if (R_SUCCEEDED(hidIsVibrationDeviceMounted(g_vibration_handles[i], &mounted)) && mounted)
      hidSendVibrationValue(g_vibration_handles[i], value);
  }
}

static void vibration_start(int length_ms, float low, float high) {
  if (length_ms <= 0) return;
  if (length_ms > 1000) length_ms = 1000;
  HidVibrationValue value = { low, 160.0f, high, 320.0f };
  mutexLock(&g_vibration_lock);
  vibration_send(&value);
  g_vibration_deadline = armGetSystemTick() + armNsToTicks((u64)length_ms * 1000000ULL);
  g_vibration_active = 1;
  mutexUnlock(&g_vibration_lock);
}

void android_native_input_init(void){
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  mutexInit(&g_vibration_lock);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 2);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 2);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft, 1);
  vibration_add(HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight, 1);
  vibration_add(HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2);
}

void android_native_vibration_standard(int length_ms) {
  vibration_start(length_ms, 0.55f, 0.40f);
}

void android_native_vibration_haptic(int style) {
  static const struct { uint16_t ms; float low, high; } effects[] = {
    { 100, 0.90f, 0.65f }, { 75, 0.65f, 0.50f }, { 45, 0.35f, 0.30f },
    { 55, 0.45f, 0.80f },  { 85, 0.35f, 0.25f }, { 35, 0.30f, 0.65f },
    { 25, 0.20f, 0.45f },  { 120, 0.70f, 0.35f }, { 100, 0.45f, 0.70f },
    { 130, 0.70f, 0.45f }, { 180, 0.90f, 0.70f }, { 20, 0.18f, 0.35f },
    { 90, 0.25f, 0.65f },  { 140, 0.45f, 0.25f }, { 250, 0.40f, 0.30f },
    { 60, 0.50f, 0.40f },  { 75, 0.40f, 0.25f }, { 220, 0.35f, 0.18f },
    { 15, 0.12f, 0.22f },
  };
  unsigned index = (unsigned)style;
  if (index >= sizeof(effects) / sizeof(effects[0])) index = 15;
  vibration_start(effects[index].ms, effects[index].low, effects[index].high);
}

void android_native_vibration_update(void) {
  mutexLock(&g_vibration_lock);
  if (g_vibration_active && armGetSystemTick() >= g_vibration_deadline) {
    HidVibrationValue stop = { 0.0f, 160.0f, 0.0f, 320.0f };
    vibration_send(&stop);
    g_vibration_active = 0;
  }
  mutexUnlock(&g_vibration_lock);
}

void android_native_vibration_shutdown(void) {
  HidVibrationValue stop = { 0.0f, 160.0f, 0.0f, 320.0f };
  mutexLock(&g_vibration_lock);
  vibration_send(&stop);
  g_vibration_active = 0;
  mutexUnlock(&g_vibration_lock);
}

/* nativeInjectEvent(env, thiz, event, flags). */
typedef uint8_t (*inject_fn)(void*,void*,void*,int);

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  padUpdate(&g_pad);

  int n = hidGetTouchScreenStates(&g_touch, 1);
  if (n > 0 && g_touch.count > 0){
    int   ids[UI_MAX_POINTERS]; float xs[UI_MAX_POINTERS]; float ys[UI_MAX_POINTERS];
    int c = g_touch.count > UI_MAX_POINTERS ? UI_MAX_POINTERS : g_touch.count;
    const float PANEL_W = 1280.0f, PANEL_H = 720.0f;
    for (int i=0;i<c;i++){ ids[i]=(int)g_touch.touches[i].finger_id;
      float px=(float)g_touch.touches[i].x, py=(float)g_touch.touches[i].y;
      xs[i]=px * ((float)g_w / PANEL_W);
      ys[i]=py * ((float)g_h / PANEL_H);
    }
    g_last_tx = xs[0]; g_last_ty = ys[0];
    int action = g_prev_touch ? AMOTION_ACTION_MOVE : AMOTION_ACTION_DOWN;
    inject(env, thiz, unity_motionevent(action, c, ids, xs, ys), 0);
    g_prev_touch = c;
    return;
  }
  if (g_prev_touch){
    int   ids[1]={0}; float xs[1]={g_last_tx}, ys[1]={g_last_ty};
    inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    g_prev_touch = 0;
    return;
  }

  HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
  g_cursor_x += (ls.x / 32767.0f) * 14.0f;
  g_cursor_y -= (ls.y / 32767.0f) * 14.0f;
  if (g_cursor_x < 0) g_cursor_x = 0;
  if (g_cursor_x > g_w) g_cursor_x = g_w;
  if (g_cursor_y < 0) g_cursor_y = 0;
  if (g_cursor_y > g_h) g_cursor_y = g_h;

  int a = (padGetButtons(&g_pad) & HidNpadButton_A) ? 1 : 0;
  int ids[1]={0}; float xs[1]={g_cursor_x}, ys[1]={g_cursor_y};
  if (a && !g_prev_a)      inject(env, thiz, unity_motionevent(AMOTION_ACTION_DOWN, 1, ids, xs, ys), 0);
  else if (a && g_prev_a)  inject(env, thiz, unity_motionevent(AMOTION_ACTION_MOVE, 1, ids, xs, ys), 0);
  else if (!a && g_prev_a) inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP,   1, ids, xs, ys), 0);
  g_prev_a = a;

  static int prev_b = 0;
  int b = (padGetButtons(&g_pad) & HidNpadButton_B) ? 1 : 0;
  if (b && !prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
  if (!b && prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
  prev_b = b;
}
