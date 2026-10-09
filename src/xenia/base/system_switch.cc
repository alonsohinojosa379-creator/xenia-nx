/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/system.h"

#include <cstdio>

#include "xenia/base/platform.h"

namespace xe {

// There is no desktop to open a browser or file manager on.
void LaunchWebBrowser(const std::string_view url) {}

void LaunchFileExplorer(const std::filesystem::path& path) {}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  // The front-ends own the console; keep this to stderr so it shows up in
  // nxlink and does not corrupt their screens.
  const char* prefix = "";
  switch (type) {
    case SimpleMessageBoxType::Help:
      prefix = "";
      break;
    case SimpleMessageBoxType::Warning:
      prefix = "Warning: ";
      break;
    case SimpleMessageBoxType::Error:
      prefix = "Error: ";
      break;
  }
  fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(message.size()),
          message.data());
  fflush(stderr);
}

}  // namespace xe
