/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// POSIX functions that Xenia (and the bundled xbyak_aarch64) call but that
// devkitA64's newlib does not implement.

#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

extern "C" {

long sysconf(int name) {
  switch (name) {
    case _SC_PAGESIZE:
      return 0x1000;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
      // Applications get 3 cores of the Tegra X1 (the 4th is reserved for the
      // system) but a fourth is available in some configurations.
      return 4;
    default:
      errno = EINVAL;
      return -1;
  }
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
  if (!memptr || alignment < sizeof(void*) ||
      (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  void* ptr = memalign(alignment, size ? size : 1);
  if (!ptr) {
    return ENOMEM;
  }
  *memptr = ptr;
  return 0;
}

}  // extern "C"
