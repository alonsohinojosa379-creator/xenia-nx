/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Xenia NX - PowerPC instruction test harness for Nintendo Switch.
//
// Stage 1 bring-up target. Runs Xenia's PPC instruction test-suite
// (src/xenia/cpu/ppc/testing) through the PPC -> ARM64 JIT on the console.
// If these pass, the CPU core, guest memory layout and fault handling work on
// Horizon.
//
// Differences from the desktop runner (ppc_testing_main.cc):
//   - No fork(): tests run in-process. To survive a crash the harness writes
//     the name of the running test to `current.txt` first; on the next launch
//     that test is recorded as CRASH and the run continues after it.
//   - Test data comes from the SD card (see kRoot): <suite>.bin / <suite>.map
//     (assembled by CI) and <suite>.s (only the `test_*:` labels and the
//     `#_` annotations are needed).
//   - Output goes to the on-screen console and to ppc-tests.log.
//
// Controls: hold ZL+ZR at launch to discard saved progress, + to exit.

#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"

DECLARE_path(log_file);
DECLARE_bool(log_to_stdout);

namespace xe {
namespace switch_nx {

using namespace xe::literals;
using xe::cpu::Processor;
using xe::cpu::ThreadState;
using xe::cpu::ppc::PPCContext;

constexpr uint32_t kStartAddress = 0x80000000;

constexpr const char* kRoot = "sdmc:/switch/xenia-nx";
constexpr const char* kTestsDir = "sdmc:/switch/xenia-nx/tests";
constexpr const char* kLogPath = "sdmc:/switch/xenia-nx/ppc-tests.log";
constexpr const char* kXeniaLogPath = "sdmc:/switch/xenia-nx/xenia-nx.log";
constexpr const char* kDonePath = "sdmc:/switch/xenia-nx/ppc-tests.done.txt";
constexpr const char* kCurrentPath =
    "sdmc:/switch/xenia-nx/ppc-tests.current.txt";
constexpr const char* kSkipPath = "sdmc:/switch/xenia-nx/tests/skip.txt";

// ---------------------------------------------------------------------------
// Output.
// ---------------------------------------------------------------------------

static FILE* g_log = nullptr;

static void SyncFile(FILE* f) {
  fflush(f);
  fsync(fileno(f));
}

// Prints to the console and the log file.
static void Out(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  fputs(buffer, stdout);
  if (g_log) {
    fputs(buffer, g_log);
    SyncFile(g_log);
  }
  consoleUpdate(nullptr);
}

// Log file only (verbose failure details).
static void LogOnly(const char* fmt, ...) {
  if (!g_log) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vfprintf(g_log, fmt, args);
  va_end(args);
  SyncFile(g_log);
}

// ---------------------------------------------------------------------------
// Small file helpers.
// ---------------------------------------------------------------------------

static void MakeDirs() {
  mkdir("sdmc:/switch", 0777);
  mkdir(kRoot, 0777);
  mkdir(kTestsDir, 0777);
}

static bool ReadLines(const std::string& path, std::vector<std::string>& out) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) {
    return false;
  }
  // Annotation lines (vector memory dumps) can be long.
  std::vector<char> buffer(64 * 1024);
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), f)) {
    std::string line(buffer.data());
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                             line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    out.push_back(std::move(line));
  }
  fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// Test suite loading (same formats as the desktop runner).
// ---------------------------------------------------------------------------

typedef std::vector<std::pair<std::string, std::string>> AnnotationList;

struct TestCase {
  TestCase(uint32_t address, std::string name)
      : address(address), name(std::move(name)) {}
  uint32_t address;
  std::string name;
  AnnotationList annotations;
};

class TestSuite {
 public:
  explicit TestSuite(const std::string& name) : name_(name) {
    std::string base = std::string(kTestsDir) + "/" + name;
    src_path_ = base + ".s";
    map_path_ = base + ".map";
    bin_path_ = base + ".bin";
  }

  bool Load() {
    if (!ReadMap()) {
      LogOnly("[%s] unable to read map %s\n", name_.c_str(),
              map_path_.c_str());
      return false;
    }
    if (!ReadAnnotations()) {
      LogOnly("[%s] unable to read annotations %s\n", name_.c_str(),
              src_path_.c_str());
      return false;
    }
    return true;
  }

  const std::string& name() const { return name_; }
  const std::string& bin_path() const { return bin_path_; }
  std::vector<TestCase>& test_cases() { return test_cases_; }

 private:
  TestCase* FindTestCase(const std::string& name) {
    for (auto& test_case : test_cases_) {
      if (test_case.name == name) {
        return &test_case;
      }
    }
    return nullptr;
  }

  bool ReadMap() {
    std::vector<std::string> lines;
    if (!ReadLines(map_path_, lines)) {
      return false;
    }
    for (auto& line : lines) {
      // 0000000000000000 t test_add1
      size_t pos = line.find(" t test_");
      if (pos == std::string::npos) {
        continue;
      }
      std::string address = line.substr(0, pos);
      std::string name = line.substr(pos + strlen(" t test_"));
      test_cases_.emplace_back(
          kStartAddress +
              static_cast<uint32_t>(std::strtoul(address.c_str(), nullptr, 16)),
          name);
    }
    return true;
  }

  bool ReadAnnotations() {
    std::vector<std::string> lines;
    if (!ReadLines(src_path_, lines)) {
      return false;
    }
    TestCase* current = nullptr;
    for (auto& raw : lines) {
      size_t first = raw.find_first_not_of(' ');
      if (first == std::string::npos) {
        continue;
      }
      const char* start = raw.c_str() + first;
      if (strncmp(start, "test_", 5) == 0) {
        const char* colon = strchr(start, ':');
        if (!colon) {
          continue;
        }
        std::string label(start + 5, colon);
        current = FindTestCase(label);
        if (!current) {
          LogOnly("[%s] test case %s not found in map\n", name_.c_str(),
                  label.c_str());
          return false;
        }
      } else if (strlen(start) > 3 && start[0] == '#' && start[1] == '_') {
        const char* space = strchr(start + 3, ' ');
        if (!space) {
          continue;
        }
        std::string key(start + 3, space);
        std::string value(space + 1);
        if (!current) {
          LogOnly("[%s] annotation outside of a test case\n", name_.c_str());
          return false;
        }
        current->annotations.emplace_back(std::move(key), std::move(value));
      }
    }
    return true;
  }

  std::string name_;
  std::string src_path_;
  std::string map_path_;
  std::string bin_path_;
  std::vector<TestCase> test_cases_;
};

// ---------------------------------------------------------------------------
// Runner (adapted from the desktop runner).
// ---------------------------------------------------------------------------

class TestRunner {
 public:
  TestRunner() {
    memory_.reset(new Memory());
    initialized_ = memory_->Initialize();
  }

  ~TestRunner() {
    thread_state_.reset();
    processor_.reset();
    memory_.reset();
  }

  bool initialized() const { return initialized_; }

  bool Setup(TestSuite& suite) {
    thread_state_.reset();
    memory_->Reset();

    std::unique_ptr<xe::cpu::backend::Backend> backend(
        new xe::cpu::backend::a64::A64Backend());

    processor_.reset(new Processor(memory_.get(), nullptr));
    processor_->Setup(std::move(backend));
    processor_->set_debug_info_flags(cpu::DebugInfoFlags::kDebugInfoAll);

    auto module = std::make_unique<xe::cpu::RawModule>(processor_.get());
    if (!module->LoadFile(kStartAddress, suite.bin_path())) {
      LogOnly("unable to load test binary %s\n", suite.bin_path().c_str());
      return false;
    }
    processor_->AddModule(std::move(module));

    processor_->backend()->CommitExecutableRange(kStartAddress,
                                                 kStartAddress + 1024 * 1024);

    // Dummy space for tests that touch memory.
    processor_->memory()->LookupHeap(0)->AllocFixed(
        0x10001000, 0xEFFF, 0,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite);

    // Simulate a thread.
    uint32_t stack_size = 64 * 1024;
    uint32_t stack_address = kStartAddress - stack_size;
    uint32_t pcr_address = stack_address - 0x1000;
    thread_state_.reset(
        new ThreadState(processor_.get(), 0x100, stack_address, pcr_address));
    return true;
  }

  bool Run(TestCase& test_case) {
    SetupTestState(test_case);

    // Reset FPCR and backend flags to the default FPU state before each test
    // (a previous VMX test may leave FZ|DN set).
    {
      auto* a64_backend = static_cast<xe::cpu::backend::a64::A64Backend*>(
          processor_->backend());
      auto* bctx =
          a64_backend->BackendContextForGuestContext(thread_state_->context());
      bctx->flags &= ~(1U << xe::cpu::backend::a64::kA64BackendFPCRModeBit);
      a64_backend->SetGuestRoundingMode(thread_state_->context(), 0);
    }

    auto fn = processor_->ResolveFunction(test_case.address);
    if (!fn) {
      LogOnly("    [%s] entry function not found\n", test_case.name.c_str());
      return false;
    }

    auto ctx = thread_state_->context();
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state_.get(), uint32_t(ctx->lr));

    return CheckTestResults(test_case);
  }

 private:
  void SetupTestState(TestCase& test_case) {
    auto ppc_context = thread_state_->context();
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_IN") {
        size_t space_pos = it.second.find(' ');
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        ppc_context->SetRegFromString(reg_name.c_str(), reg_value.c_str());
      } else if (it.first == "MEMORY_IN") {
        size_t space_pos = it.second.find(' ');
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto p = memory_->TranslateVirtual(address);
        const char* c = bytes_str.c_str();
        while (*c) {
          while (*c == ' ') {
            ++c;
          }
          if (!*c) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          *p = static_cast<uint8_t>(std::strtoul(ccs, nullptr, 16));
          ++p;
        }
      }
    }
  }

  bool CheckTestResults(TestCase& test_case) {
    auto ppc_context = thread_state_->context();
    bool any_failed = false;
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_OUT") {
        size_t space_pos = it.second.find(' ');
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        std::string actual_value;
        if (!ppc_context->CompareRegWithString(
                reg_name.c_str(), reg_value.c_str(), actual_value)) {
          any_failed = true;
          LogOnly("    [%s] register %s: expected %s, got %s\n",
                  test_case.name.c_str(), reg_name.c_str(), reg_value.c_str(),
                  actual_value.c_str());
        }
      } else if (it.first == "MEMORY_OUT") {
        size_t space_pos = it.second.find(' ');
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto base_address = memory_->TranslateVirtual(address);
        auto p = base_address;
        const char* c = bytes_str.c_str();
        bool failed = false;
        std::string expecteds;
        std::string actuals;
        while (*c) {
          while (*c == ' ') {
            ++c;
          }
          if (!*c) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          uint32_t expected = std::strtoul(ccs, nullptr, 16);
          uint8_t actual = *p;
          char tmp[8];
          snprintf(tmp, sizeof(tmp), " %02X", expected);
          expecteds += tmp;
          snprintf(tmp, sizeof(tmp), " %02X", actual);
          actuals += tmp;
          if (expected != actual) {
            any_failed = true;
            failed = true;
          }
          ++p;
        }
        if (failed) {
          LogOnly("    [%s] memory %s\n      expected:%s\n        actual:%s\n",
                  test_case.name.c_str(), address_str.c_str(),
                  expecteds.c_str(), actuals.c_str());
        }
      }
    }
    return !any_failed;
  }

  bool initialized_ = false;
  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Processor> processor_;
  std::unique_ptr<ThreadState> thread_state_;
};

// ---------------------------------------------------------------------------
// Progress tracking (crash recovery without fork).
// ---------------------------------------------------------------------------

enum class Result { kPass, kFail, kCrash };

static const char* ResultName(Result r) {
  switch (r) {
    case Result::kPass:
      return "PASS";
    case Result::kFail:
      return "FAIL";
    case Result::kCrash:
      return "CRASH";
  }
  return "?";
}

static bool ParseResult(const std::string& s, Result& out) {
  if (s == "PASS") {
    out = Result::kPass;
  } else if (s == "FAIL") {
    out = Result::kFail;
  } else if (s == "CRASH") {
    out = Result::kCrash;
  } else {
    return false;
  }
  return true;
}

static void AppendDone(const std::string& key, Result r) {
  FILE* f = fopen(kDonePath, "a");
  if (!f) {
    return;
  }
  fprintf(f, "%s\t%s\n", key.c_str(), ResultName(r));
  SyncFile(f);
  fclose(f);
}

static void WriteCurrent(const std::string& key) {
  FILE* f = fopen(kCurrentPath, "w");
  if (!f) {
    return;
  }
  fputs(key.c_str(), f);
  SyncFile(f);
  fclose(f);
}

static void ClearCurrent() { unlink(kCurrentPath); }

static std::vector<std::string> DiscoverSuites() {
  std::vector<std::string> names;
  DIR* dir = opendir(kTestsDir);
  if (!dir) {
    return names;
  }
  while (dirent* entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name.size() > 2 && name.compare(0, 6, "instr_") == 0 &&
        name.compare(name.size() - 2, 2, ".s") == 0) {
      names.push_back(name.substr(0, name.size() - 2));
    }
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

static bool ShouldExit() { return !appletMainLoop(); }

static int RunAll(PadState& pad) {
  // Previous progress.
  std::map<std::string, Result> done;
  {
    std::vector<std::string> lines;
    if (ReadLines(kDonePath, lines)) {
      for (auto& line : lines) {
        size_t tab = line.find('\t');
        Result r;
        if (tab != std::string::npos &&
            ParseResult(line.substr(tab + 1), r)) {
          done[line.substr(0, tab)] = r;
        }
      }
    }
  }

  // A leftover current.txt means the last run died inside that test.
  {
    std::vector<std::string> lines;
    if (ReadLines(kCurrentPath, lines) && !lines.empty() &&
        !lines[0].empty()) {
      const std::string& key = lines[0];
      if (!done.count(key)) {
        done[key] = Result::kCrash;
        AppendDone(key, Result::kCrash);
        Out("Previous run crashed in: %s\n", key.c_str());
      }
    }
    ClearCurrent();
  }

  std::set<std::string> skip_list;
  {
    std::vector<std::string> lines;
    ReadLines(kSkipPath, lines);
    for (auto& line : lines) {
      if (!line.empty() && line[0] != '#') {
        skip_list.insert(line);
      }
    }
  }

  auto suites = DiscoverSuites();
  if (suites.empty()) {
    Out("No tests found in %s\n", kTestsDir);
    Out("Copy the 'tests' folder from the release zip to the SD card.\n");
    return 2;
  }
  Out("%zu test files, %zu results from earlier runs.\n", suites.size(),
      done.size());

  TestRunner runner;
  if (!runner.initialized()) {
    Out("FATAL: guest memory initialization failed (see xenia-nx.log).\n");
    return 3;
  }
  Out("Guest memory OK. Running...\n\n");

  int pass = 0, fail = 0, crash = 0, skipped = 0, ran = 0;
  std::vector<std::string> bad;

  auto tally = [&](const std::string& key, Result r) {
    switch (r) {
      case Result::kPass:
        ++pass;
        break;
      case Result::kFail:
        ++fail;
        bad.push_back(key + " FAIL");
        break;
      case Result::kCrash:
        ++crash;
        bad.push_back(key + " CRASH");
        break;
    }
  };

  for (auto& suite_name : suites) {
    if (ShouldExit()) {
      Out("Exit requested; progress saved.\n");
      return 1;
    }
    TestSuite suite(suite_name);
    if (!suite.Load()) {
      Out("[%s] failed to load\n", suite_name.c_str());
      continue;
    }

    int suite_pass = 0, suite_bad = 0;
    for (auto& test_case : suite.test_cases()) {
      std::string key = suite_name + "/" + test_case.name;
      if (skip_list.count(test_case.name)) {
        ++skipped;
        continue;
      }
      auto prev = done.find(key);
      if (prev != done.end()) {
        tally(key, prev->second);
        if (prev->second == Result::kPass) {
          ++suite_pass;
        } else {
          ++suite_bad;
        }
        continue;
      }

      WriteCurrent(key);
      Result r;
      if (!runner.Setup(suite)) {
        r = Result::kFail;
        LogOnly("    [%s] setup failed\n", key.c_str());
      } else {
        r = runner.Run(test_case) ? Result::kPass : Result::kFail;
      }
      ClearCurrent();

      AppendDone(key, r);
      done[key] = r;
      tally(key, r);
      ++ran;
      if (r == Result::kPass) {
        ++suite_pass;
      } else {
        ++suite_bad;
        LogOnly("%s %s\n", ResultName(r), key.c_str());
      }
    }
    Out("%-28s ok %3d  bad %d\n", suite_name.c_str(), suite_pass, suite_bad);

    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
      Out("Stopped by user; progress saved.\n");
      return 1;
    }
  }

  Out("\n==== Summary ====\n");
  Out("Passed : %d\nFailed : %d\nCrashed: %d\nSkipped: %d\n", pass, fail, crash,
      skipped);
  Out("(%d tests executed this launch)\n", ran);
  if (!bad.empty()) {
    Out("\nNot passing (first 25, all in the log):\n");
    for (size_t i = 0; i < bad.size(); ++i) {
      if (i < 25) {
        Out("  %s\n", bad[i].c_str());
      } else {
        LogOnly("  %s\n", bad[i].c_str());
      }
    }
  }
  return (fail || crash) ? 1 : 0;
}

}  // namespace switch_nx
}  // namespace xe

int main(int argc, char** argv) {
  using namespace xe::switch_nx;

  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  padUpdate(&pad);

  MakeDirs();

  // Hold ZL+ZR at launch to start from scratch.
  {
    u64 held = padGetButtons(&pad);
    const u64 both = HidNpadButton_ZL | HidNpadButton_ZR;
    if ((held & both) == both) {
      unlink(kDonePath);
      unlink(kCurrentPath);
      printf("Saved progress cleared.\n");
    }
  }

  g_log = fopen(kLogPath, "a");

  // Xenia's own logging goes to a file, not the console.
  cvars::log_file = std::filesystem::path(kXeniaLogPath);
  cvars::log_to_stdout = false;
  xe::InitializeLogging("xenia-nx");

  Out("Xenia NX - PPC tests (stage 1)\n");
  Out("ARM64 JIT, guest memory and fault handling\n\n");

  int code = RunAll(pad);

  Out("\nPress + to exit.\n");
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
      break;
    }
    consoleUpdate(nullptr);
  }

  xe::ShutdownLogging();
  if (g_log) {
    fclose(g_log);
    g_log = nullptr;
  }
  consoleExit(nullptr);
  return code;
}
