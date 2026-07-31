/* Unity 2022.3.62f2 entry points used by Subway Surfers 3.66.1. */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

#define OFF_JNI_OnLoad                     0x727dd8
#define OFF_initJni                        0x726fd4
#define OFF_nativeRecreateGfxState         0x727208
#define OFF_nativeSendSurfaceChangedEvent  0x727270
#define OFF_nativeRender                   0x7272c8
#define OFF_nativeInjectEvent              0x727328
#define OFF_nativeResume                   0x7270d4
#define OFF_nativeFocusChanged             0x7271b4
#define OFF_nativeDone                     0x726fe0
#define OFF_nativeApplicationUnload        0x727164

#define OFF_TimeManager_Update_entry       0x569298
#define OFF_TimeManager_Update_body        0x5692bc

typedef void    (*fn_initJni)(void *, void *, void *);
typedef void    (*fn_gfxstate)(void *, void *, int32_t, void *);
typedef void    (*fn_v)(void *, void *);
typedef uint8_t (*fn_z)(void *, void *);
typedef void    (*fn_vz)(void *, void *, int32_t);
typedef uint8_t (*fn_inject)(void *, void *, void *, int32_t);

#define UNITY_RESOLVE(mod, off) ((void *)((uintptr_t)(mod).load_virtbase + (off)))

#endif
