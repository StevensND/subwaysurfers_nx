/* Android NDK window, looper, sensor, and input shims used by libunity. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <switch.h>
#include "nx_pointer.h"   /* right-stick cursor / mouse support (menus) */

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
static float g_last_tx = 640, g_last_ty = 360;
static int   g_prev_a = 0;
static int   g_prev_x = 0;
static int   g_prev_y = 0;
static int   g_prev_pause = 0;
static int   g_prev_r = 0;               /* R last frame, for edge detection */
static int   g_dtap_step = -1;           /* -1 = idle; 0..N = double-tap sequence frame */

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

/* Lazy one-time init of the right-stick cursor module. Called at the top of
 * android_native_feed_hid so it runs once the window geometry (g_w/g_h) is known.
 * Subway Surfers renders landscape, so rotation is 0. The host keeps its own
 * touch handling, so handle_touch is 0. Settings/cursor.png live under the port's
 * SD folder; file I/O goes through the port's locked fopen/fclose wrappers. */
extern FILE *fopen_fake(const char *path, const char *mode);
extern int   fclose_fake(FILE *f);
static void nxp_ensure_init(void){
  static int done = 0;
  if (done) return;
  done = 1;
  NxpConfig c = {0};
  c.screen_w = (int)g_w; c.screen_h = (int)g_h;   /* render space              */
  c.panel_w  = 1280;     c.panel_h  = 720;         /* Switch touch panel        */
  c.data_dir = "sdmc:/switch/subwaysurfers_nx";    /* cursor.png / pointer.cfg  */
  c.rotation = 0;                                  /* landscape, no rotation    */
  c.handle_touch = 0;                              /* host keeps its own touch  */
  c.cursor_id = 0; c.max_touch_slots = UI_MAX_POINTERS;
  c.fopen_fn = fopen_fake; c.fclose_fn = fclose_fake;
  nxp_init(&c);
}

/* --- Gamepad -> swipe gestures --------------------------------------------- *
 * Subway Surfers is driven by swipes (up=jump, down=roll, left/right=lane), not
 * by a cursor. We turn a D-pad press or a stick flick into a short synthetic
 * swipe: an ACTION_DOWN at a start point, a few ACTION_MOVE steps toward the
 * direction, then an ACTION_UP. The game reads the delta and direction. A swipe
 * plays out over several frames so the motion is registered as a gesture, not a
 * tap. Only one swipe runs at a time; new direction presses are ignored until the
 * current swipe finishes, which matches how the game consumes them. */
enum { SWIPE_NONE = 0, SWIPE_UP, SWIPE_DOWN, SWIPE_LEFT, SWIPE_RIGHT };

static int   g_swipe_dir   = SWIPE_NONE;
static int   g_swipe_step  = -1;        /* -1 = not swiping */
static int   g_prev_dir    = SWIPE_NONE; /* direction last frame, for edge detection */
#define SWIPE_STEPS 3                    /* frames from DOWN to UP (shorter = snappier) */

static int stick_dir(HidAnalogStickState ls) {
  /* Deadzones as a fraction of full range (max 32767).
   *
   * Tuning learned from testing:
   *  - Soft flicks UP weren't detected (e.g. a thumb sliding up off the stick). A
   *    resting thumb drifts DOWN, not up, so an upward tilt is almost always
   *    intentional -- UP can therefore have a LOW deadzone (DZ_UP) and still not
   *    misfire. DOWN keeps a HIGH deadzone (DZ_DOWN) so resting-thumb drift doesn't
   *    register as a phantom roll. This asymmetry catches soft up-flicks while
   *    avoiding phantom downs.
   *  - Soft moves RIGHT/LEFT also need a gentler threshold, so DZ_H is a bit lower.
   *  - Horizontal is biased (H_BIAS): the vertical component must beat the
   *    horizontal one by a margin before the vertical axis wins, so a diagonal
   *    right-up push resolves to right (lane change), not up. */
  const int DZ_H    = 8000;    /* ~24% horizontal -- catch soft L/R                 */
  const int DZ_UP   = 9000;    /* ~27% up -- catch soft up-flicks (thumb won't drift up) */
  const int DZ_DOWN = 20000;   /* ~61% down -- deliberate, avoids phantom roll      */
  const int H_BIAS  = 9000;    /* vertical must beat horizontal by this to win -- keeps
                                * a rightward move (even a soft one) from being read as
                                * up when the thumb drifts up diagonally */
  int ax = ls.x, ay = ls.y;
  int aax = ax < 0 ? -ax : ax, aay = ay < 0 ? -ay : ay;

  /* Choose the vertical axis only when it clearly dominates; otherwise horizontal.
   * Up and down use different deadzones. */
  if (aay > aax + H_BIAS) {
    if (ay > 0) return aay >= DZ_UP   ? SWIPE_UP   : SWIPE_NONE;   /* stick up = +y */
    else        return aay >= DZ_DOWN ? SWIPE_DOWN : SWIPE_NONE;
  } else {
    if (aax < DZ_H) return SWIPE_NONE;
    return ax > 0 ? SWIPE_RIGHT : SWIPE_LEFT;
  }
}

static int button_dir(u64 btn) {
  if (btn & (HidNpadButton_Up    | HidNpadButton_StickLUp    | HidNpadButton_StickRUp))    return SWIPE_UP;
  if (btn & (HidNpadButton_Down  | HidNpadButton_StickLDown  | HidNpadButton_StickRDown))  return SWIPE_DOWN;
  if (btn & (HidNpadButton_Left  | HidNpadButton_StickLLeft  | HidNpadButton_StickRLeft))  return SWIPE_LEFT;
  if (btn & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)) return SWIPE_RIGHT;
  return SWIPE_NONE;
}

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  nxp_ensure_init();
  padUpdate(&g_pad);

  /* Real touchscreen always wins, so handheld touch keeps working. */
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

  u64 btn = padGetButtons(&g_pad);

  /* Right-stick cursor (menus). nx_pointer reads the right stick, handles the R3
   * toggle, and emits pointer events already in screen space; inject each as a
   * single-pointer motion so taps register like a finger. This runs every frame
   * regardless of the swipe/character logic below. When the cursor is visible the
   * right stick is claimed for it (see the movement code above), so it won't also
   * move the character. Touch above returns early, so this only runs when not
   * touching. */
  nxp_update();
  {
    NxpEvent pev[8];
    int pn = nxp_poll(pev, 8);
    for (int i = 0; i < pn; i++) {
      int   ids[1] = { 0 };
      float xs[1]  = { pev[i].x }, ys[1] = { pev[i].y };
      int action = pev[i].phase == NXP_DOWN ? AMOTION_ACTION_DOWN
                 : pev[i].phase == NXP_UP   ? AMOTION_ACTION_UP
                                            : AMOTION_ACTION_MOVE;
      inject(env, thiz, unity_motionevent(action, 1, ids, xs, ys), 0);
    }
  }
  /* When the cursor is active BOTH sticks drive the pointer and the D-pad adjusts
   * cursor sensitivity, so none of them may reach character movement. Each stick
   * also reaches movement via its digital Stick* bits (read by button_dir). Mask
   * the sticks' digital bits AND the D-pad out of the button word, and skip both
   * analog reads, so nothing the cursor uses registers as a swipe. */
  u64 dbtn = btn;
  if (nxp_cursor_visible())
    dbtn &= ~(HidNpadButton_StickRUp | HidNpadButton_StickRDown |
              HidNpadButton_StickRLeft | HidNpadButton_StickRRight |
              HidNpadButton_StickLUp | HidNpadButton_StickLDown |
              HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
              HidNpadButton_Up | HidNpadButton_Down |
              HidNpadButton_Left | HidNpadButton_Right);
  int dir = button_dir(dbtn);
  /* Analog stick reads only when the cursor is NOT active (it owns both sticks). */
  if (dir == SWIPE_NONE && !nxp_cursor_visible())
    dir = stick_dir(padGetStickPos(&g_pad, 0));  /* left stick  */
  if (dir == SWIPE_NONE && !nxp_cursor_visible())
    dir = stick_dir(padGetStickPos(&g_pad, 1));  /* right stick */

  /* Start a new swipe when the input direction CHANGES to a non-neutral value.
   * This fires on neutral->dir (a fresh press) AND on dirA->dirB (flick straight
   * from one direction to another without returning to center), so you can chain
   * up-then-left instantly. Holding the SAME direction does not re-fire, which is
   * what kept the old code from double-moving. A new direction also interrupts a
   * swipe already in progress. */
  if (dir != SWIPE_NONE && dir != g_prev_dir) {
    /* If a previous swipe was mid-sequence, close it with an UP at center so the
     * game does not see a stuck finger, then begin the new one. */
    if (g_swipe_step >= 0 && g_swipe_dir != SWIPE_NONE) {
      int ids[1] = {0}; float xs[1] = { g_w * 0.5f }, ys[1] = { g_h * 0.5f };
      inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    }
    g_swipe_dir  = dir;
    g_swipe_step = 0;           /* restart the sequence for the new direction */
  }
  g_prev_dir = dir;

  /* Emit the current step of the active swipe, if any. The sequence is short so
   * it never blocks the next input for long: DOWN, a couple of MOVEs, then UP. */
  if (g_swipe_step >= 0 && g_swipe_dir != SWIPE_NONE) {
    const float cx = g_w * 0.5f, cy = g_h * 0.5f;
    const float REACH = (g_swipe_dir == SWIPE_LEFT || g_swipe_dir == SWIPE_RIGHT)
                          ? g_w * 0.30f : g_h * 0.30f;
    float dx = 0, dy = 0;
    if (g_swipe_dir == SWIPE_LEFT)  dx = -REACH;
    if (g_swipe_dir == SWIPE_RIGHT) dx =  REACH;
    if (g_swipe_dir == SWIPE_UP)    dy = -REACH;   /* screen y grows downward */
    if (g_swipe_dir == SWIPE_DOWN)  dy =  REACH;

    float t = (float)g_swipe_step / (float)SWIPE_STEPS;   /* 0 .. 1 */
    int   ids[1] = {0};
    float xs[1]  = { cx + dx * t };
    float ys[1]  = { cy + dy * t };

    int action;
    if (g_swipe_step == 0)                action = AMOTION_ACTION_DOWN;
    else if (g_swipe_step >= SWIPE_STEPS) action = AMOTION_ACTION_UP;
    else                                  action = AMOTION_ACTION_MOVE;

    inject(env, thiz, unity_motionevent(action, 1, ids, xs, ys), 0);

    g_swipe_step++;
    if (g_swipe_step > SWIPE_STEPS) { g_swipe_step = -1; g_swipe_dir = SWIPE_NONE; }
    return;
  }

  /* B still maps to Android BACK (pause / menu back). */
  static int prev_b = 0;
  int b = (btn & HidNpadButton_B) ? 1 : 0;
  if (b && !prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
  if (!b && prev_b) inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
  prev_b = b;

  /* A taps the center of the screen (menus, "tap to continue", start). When the
   * cursor is active, nx_pointer already uses A to tap AT the cursor, so the
   * center tap is suppressed to avoid a double tap. */
  int a = ((btn & HidNpadButton_A) && !nxp_cursor_visible()) ? 1 : 0;
  if (a != g_prev_a) {
    const float cx = g_w * 0.5f, cy = g_h * 0.5f;
    int ids[1] = {0}; float xs[1] = { cx }, ys[1] = { cy };
    inject(env, thiz,
           unity_motionevent(a ? AMOTION_ACTION_DOWN : AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    g_prev_a = a;
  }

  /* X and Y tap the two run-start power-up buttons in the lower-left corner, so
   * they can be activated with the controller. Positions are fractions of the
   * screen (measured from a 1280x720 frame), so they hold in both handheld and
   * docked. X = Score Booster (upper), Y = Headstart (lower). */
  int x = (btn & HidNpadButton_X) ? 1 : 0;
  if (x != g_prev_x) {
    const float px = g_w * 0.0375f, py = g_h * 0.6458f;   /* (48, 465) / (1280, 720) */
    int ids[1] = {0}; float xs[1] = { px }, ys[1] = { py };
    inject(env, thiz,
           unity_motionevent(x ? AMOTION_ACTION_DOWN : AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    g_prev_x = x;
  }

  int y = (btn & HidNpadButton_Y) ? 1 : 0;
  if (y != g_prev_y) {
    const float px = g_w * 0.0438f, py = g_h * 0.8097f;   /* (56, 583) / (1280, 720) */
    int ids[1] = {0}; float xs[1] = { px }, ys[1] = { py };
    inject(env, thiz,
           unity_motionevent(y ? AMOTION_ACTION_DOWN : AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    g_prev_y = y;
  }

  /* Minus and Plus tap the pause icon in the upper-left corner. Either button
   * works. Position is a fraction of the screen (measured from 1280x720), so it
   * holds in both handheld and docked. */
  int pause = (btn & (HidNpadButton_Minus | HidNpadButton_Plus)) ? 1 : 0;
  if (pause != g_prev_pause) {
    const float px = g_w * 0.0297f, py = g_h * 0.0528f;   /* (38, 38) / (1280, 720) */
    int ids[1] = {0}; float xs[1] = { px }, ys[1] = { py };
    inject(env, thiz,
           unity_motionevent(pause ? AMOTION_ACTION_DOWN : AMOTION_ACTION_UP, 1, ids, xs, ys), 0);
    g_prev_pause = pause;
  }

  /* R activates the hoverboard, which the game triggers on a double-tap. A press
   * kicks off a short state machine that emits tap, tap at screen centre across a
   * few frames (DOWN, UP, gap, DOWN, UP) so the game registers a genuine double
   * tap. Edge-triggered so holding R fires it once. */
  int r = (btn & HidNpadButton_R) ? 1 : 0;
  if (r && !g_prev_r && g_dtap_step < 0) g_dtap_step = 0;   /* start on press */
  g_prev_r = r;

  if (g_dtap_step >= 0) {
    const float cx = g_w * 0.5f, cy = g_h * 0.5f;
    int ids[1] = {0}; float xs[1] = { cx }, ys[1] = { cy };
    switch (g_dtap_step) {
      case 0: inject(env, thiz, unity_motionevent(AMOTION_ACTION_DOWN, 1, ids, xs, ys), 0); break; /* tap 1 down */
      case 1: inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP,   1, ids, xs, ys), 0); break; /* tap 1 up   */
      case 2: /* gap frame, nothing */ break;
      case 3: inject(env, thiz, unity_motionevent(AMOTION_ACTION_DOWN, 1, ids, xs, ys), 0); break; /* tap 2 down */
      case 4: inject(env, thiz, unity_motionevent(AMOTION_ACTION_UP,   1, ids, xs, ys), 0); break; /* tap 2 up   */
    }
    g_dtap_step++;
    if (g_dtap_step > 4) g_dtap_step = -1;   /* sequence done */
  }
}
