/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

/* Engine threads read the stack canary from TPIDR_EL0 + 0x28. */
#define BIONIC_TLS_SIZE 0x400
void install_bionic_tls(void *buf);

#endif
