/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "util.h"

void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  void *tp = (uint8_t *)buf + 0x200;
  __asm__ ("msr s3_3_c13_c0_2, %0" : : "r"(tp));
}
