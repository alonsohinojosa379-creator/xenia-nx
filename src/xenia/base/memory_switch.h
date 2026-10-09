/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_MEMORY_SWITCH_H_
#define XENIA_BASE_MEMORY_SWITCH_H_

#include <cstdint>

#include "xenia/base/platform.h"

#if XE_PLATFORM_SWITCH

namespace xe {
namespace memory {

// Horizon enforces W^X, so executable memory from xe::memory is RX only and
// has a second, RW mapping of the same pages. Returns the RW address
// corresponding to `address` (or `address` itself for non-executable memory),
// or nullptr if `address` isn't managed by xe::memory.
void* GetSwitchWritableAlias(void* address);

// Called by the exception handler on a data abort: backs the chunk containing
// `fault_address` if it belongs to a reserved-but-untouched view (the
// equivalent of a page of a sparse memfd being faulted in). Returns true if
// the access can be retried.
bool HandleSwitchLazyFault(uintptr_t fault_address);

}  // namespace memory
}  // namespace xe

#endif  // XE_PLATFORM_SWITCH

#endif  // XENIA_BASE_MEMORY_SWITCH_H_
