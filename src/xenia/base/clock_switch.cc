/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <sys/time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"

#include <switch.h>

namespace xe {

// Horizon: the ARM generic timer (CNTPCT_EL0, 19.2 MHz) is readable from EL0
// and is what the system itself uses for ticks.
uint64_t Clock::host_tick_frequency_platform() {
  return armGetSystemTickFreq();
}

uint64_t Clock::host_tick_count_platform() { return armGetSystemTick(); }

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 =
      ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull +
      now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

uint64_t Clock::QueryHostInterruptTime() { return host_tick_count_platform(); }
}  // namespace xe
