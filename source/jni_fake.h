/* Minimal JNI environment used by Unity and the managed game. */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;
extern void *fake_env;

extern volatile int jni_quit_requested;

void jni_init(void);

void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

#endif
