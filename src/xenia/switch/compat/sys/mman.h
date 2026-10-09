/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Minimal <sys/mman.h> for Horizon (newlib has none).
//
// Only anonymous, address-agnostic mappings are supported, backed by the
// regular heap. This is enough for third-party code (xbyak_aarch64's scratch
// buffers) that wants "some page-aligned RW memory". Xenia's own memory
// management goes through xe::memory (memory_switch.cc), not through this.

#ifndef XENIA_SWITCH_COMPAT_SYS_MMAN_H_
#define XENIA_SWITCH_COMPAT_SYS_MMAN_H_

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif

static inline void* mmap(void* addr, size_t length, int prot, int flags,
                         int fd, off_t offset) {
  (void)prot;
  (void)offset;
  if (fd != -1 || !(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED) || addr) {
    errno = ENOSYS;
    return MAP_FAILED;
  }
  size_t aligned = (length + 0xFFF) & ~(size_t)0xFFF;
  void* p = aligned_alloc(0x1000, aligned ? aligned : 0x1000);
  if (!p) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  memset(p, 0, aligned);
  return p;
}

static inline int munmap(void* addr, size_t length) {
  (void)length;
  free(addr);
  return 0;
}

// Heap memory is always RW and can't be made executable; report success so
// callers that only toggle RW <-> R keep working.
static inline int mprotect(void* addr, size_t length, int prot) {
  (void)addr;
  (void)length;
  if (prot & PROT_EXEC) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif  // XENIA_SWITCH_COMPAT_SYS_MMAN_H_
