/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Force-included into every translation unit on Nintendo Switch. Fills small
// gaps in newlib that Xenia's POSIX code relies on.

#ifndef XENIA_SWITCH_COMPAT_XENIA_SWITCH_COMPAT_H_
#define XENIA_SWITCH_COMPAT_XENIA_SWITCH_COMPAT_H_

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// newlib has no timegm; Horizon processes run in UTC (no TZ set), so this is
// the inverse of gmtime without relying on mktime's local time handling.
static inline time_t xe_switch_timegm(struct tm* tm) {
  static const int kDaysBeforeMonth[12] = {0,   31,  59,  90,  120, 151,
                                           181, 212, 243, 273, 304, 334};
  long long year = (long long)tm->tm_year + 1900;
  long long month = tm->tm_mon;
  year += month / 12;
  month %= 12;
  if (month < 0) {
    month += 12;
    --year;
  }
  long long y = year - 1;
  long long days = 365 * (year - 1970) + (y / 4 - 1969 / 4) -
                   (y / 100 - 1969 / 100) + (y / 400 - 1969 / 400);
  days += kDaysBeforeMonth[month];
  if (month > 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
    ++days;
  }
  days += tm->tm_mday - 1;
  return (time_t)(((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 +
                  tm->tm_sec);
}
#define timegm xe_switch_timegm

#ifdef __cplusplus
}
#endif

#endif  // XENIA_SWITCH_COMPAT_XENIA_SWITCH_COMPAT_H_
