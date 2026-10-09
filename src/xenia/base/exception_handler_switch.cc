/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// CPU exception handling for Horizon (Nintendo Switch).
//
// Horizon delivers user-mode exceptions by re-entering the process at its
// entry point with x0 = exception type and x1 = a pointer to an ExceptionInfo
// block in the (process-wide) ProcessLocalRegion; the kernel blocks every
// other faulting thread until svcReturnFromException is called. libnx's stock
// handler then runs C code on a single global exception stack, which breaks
// as soon as two threads fault close together - and Xenia faults a lot
// (lazy guest memory commits, MMIO, JIT traps).
//
// So __libnx_exception_entry is overridden here:
//
//   1. Capture: copy the full register state into a frame pushed onto the
//      faulting thread's *own* stack, point the thread at a trampoline, and
//      return from the exception immediately (releasing the kernel's
//      exception lock).
//   2. The trampoline calls xe_switch_dispatch_exception(frame) on that
//      stack: lazy commit (memory_switch.cc), then Xenia's ExceptionHandler
//      chain (MMIO, backends), exactly like the POSIX signal handler.
//   3. Restore: the trampoline executes a private BRK; the entry recognizes
//      it and hands the (possibly modified) frame back to the kernel through
//      svcReturnFromException, which restores every register precisely.

#include "xenia/base/exception_handler.h"

#include <switch.h>

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/host_thread_context.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory_switch.h"
#include "xenia/base/platform.h"

extern "C" {

struct XeSwitchExceptionFrame {
  uint64_t x[31];  // x0..x30
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  uint64_t far;
  uint32_t esr;
  uint32_t type;
  uint32_t fpsr;
  uint32_t fpcr;
  uint64_t pad;
  uint8_t v[32][16];
};

static_assert(offsetof(XeSwitchExceptionFrame, sp) == 248);
static_assert(offsetof(XeSwitchExceptionFrame, pc) == 256);
static_assert(offsetof(XeSwitchExceptionFrame, pstate) == 264);
static_assert(offsetof(XeSwitchExceptionFrame, far) == 272);
static_assert(offsetof(XeSwitchExceptionFrame, esr) == 280);
static_assert(offsetof(XeSwitchExceptionFrame, type) == 284);
static_assert(offsetof(XeSwitchExceptionFrame, fpsr) == 288);
static_assert(offsetof(XeSwitchExceptionFrame, fpcr) == 292);
static_assert(offsetof(XeSwitchExceptionFrame, v) == 304);
static_assert(sizeof(XeSwitchExceptionFrame) == 816);

void xe_switch_dispatch_exception(XeSwitchExceptionFrame* frame);
void xe_switch_exception_trampoline();

}  // extern "C"

// ExceptionInfo (aarch64) layout, see svc::aarch64::ExceptionInfo:
//   u64 r[9] @0, lr @72, sp @80, pc @88, u32 pstate @96, afsr0 @100,
//   afsr1 @104, esr @108, u64 far @112.
// Magic in x1 + this BRK immediate = "restore this frame".
#define XE_SWITCH_EXC_BRK_IMM "0x358"
#define XE_SWITCH_EXC_BRK_IMM_VALUE 0x358
// "XENIA_NX"
#define XE_SWITCH_EXC_MAGIC_ASM(reg)  \
  "movz " reg ", #0x4E58\n"           \
  "movk " reg ", #0x415F, lsl #16\n"  \
  "movk " reg ", #0x4E49, lsl #32\n"  \
  "movk " reg ", #0x5845, lsl #48\n"

asm(R"(
.section .text.__libnx_exception_entry, "ax", %progbits
.global __libnx_exception_entry
.type __libnx_exception_entry, %function
.align 4
__libnx_exception_entry:
    cbz x1, .Lxe_exc_unhandled

    // Our restore breakpoint? EC == 0x3C (BRK, AArch64) with our immediate
    // and the magic in the thread's x1.
    ldr w2, [x1, #108]
    lsr w3, w2, #26
    cmp w3, #0x3C
    b.ne .Lxe_exc_capture
    and w3, w2, #0xFFFF
    mov w4, #)" XE_SWITCH_EXC_BRK_IMM R"(
    cmp w3, w4
    b.ne .Lxe_exc_capture
    ldr x3, [x1, #8]
    )" XE_SWITCH_EXC_MAGIC_ASM("x4") R"(
    cmp x3, x4
    b.ne .Lxe_exc_capture
    ldr x2, [x1, #0]
    b .Lxe_exc_restore

.Lxe_exc_capture:
    // x2 = frame on the faulting thread's stack (below a 256 byte margin).
    ldr x2, [x1, #80]
    sub x2, x2, #256
    sub x2, x2, #816
    and x2, x2, #0xFFFFFFFFFFFFFFF0

    // x0..x8 were saved by the kernel (x0/x1 now hold type/info, x2..x4 are
    // our temporaries, x7 may have been used by hbloader).
    ldp x3, x4, [x1, #0]
    stp x3, x4, [x2, #0]
    ldp x3, x4, [x1, #16]
    stp x3, x4, [x2, #16]
    ldp x3, x4, [x1, #32]
    stp x3, x4, [x2, #32]
    ldp x3, x4, [x1, #48]
    stp x3, x4, [x2, #48]
    ldr x3, [x1, #64]
    str x3, [x2, #64]
    // x9..x29 are still live.
    str x9, [x2, #72]
    stp x10, x11, [x2, #80]
    stp x12, x13, [x2, #96]
    stp x14, x15, [x2, #112]
    stp x16, x17, [x2, #128]
    stp x18, x19, [x2, #144]
    stp x20, x21, [x2, #160]
    stp x22, x23, [x2, #176]
    stp x24, x25, [x2, #192]
    stp x26, x27, [x2, #208]
    stp x28, x29, [x2, #224]
    ldr x3, [x1, #72]
    str x3, [x2, #240]
    ldr x3, [x1, #80]
    str x3, [x2, #248]
    ldr x3, [x1, #88]
    str x3, [x2, #256]
    ldr w3, [x1, #96]
    str x3, [x2, #264]
    ldr x3, [x1, #112]
    str x3, [x2, #272]
    ldr w3, [x1, #108]
    str w3, [x2, #280]
    str w0, [x2, #284]
    mrs x3, fpsr
    str w3, [x2, #288]
    mrs x3, fpcr
    str w3, [x2, #292]
    add x3, x2, #304
    stp q0, q1, [x3, #0]
    stp q2, q3, [x3, #32]
    stp q4, q5, [x3, #64]
    stp q6, q7, [x3, #96]
    stp q8, q9, [x3, #128]
    stp q10, q11, [x3, #160]
    stp q12, q13, [x3, #192]
    stp q14, q15, [x3, #224]
    stp q16, q17, [x3, #256]
    stp q18, q19, [x3, #288]
    stp q20, q21, [x3, #320]
    stp q22, q23, [x3, #352]
    stp q24, q25, [x3, #384]
    stp q26, q27, [x3, #416]
    stp q28, q29, [x3, #448]
    stp q30, q31, [x3, #480]

    // Resume the thread in the trampoline, on its own stack, x0 = frame.
    adrp x3, xe_switch_exception_trampoline
    add x3, x3, :lo12:xe_switch_exception_trampoline
    str x3, [x1, #88]
    str x2, [x1, #80]
    str x2, [x1, #0]
    mov w0, wzr
    svc 0x28
    b .

.Lxe_exc_restore:
    // Registers the kernel restores from ExceptionInfo.
    ldp x3, x4, [x2, #0]
    stp x3, x4, [x1, #0]
    ldp x3, x4, [x2, #16]
    stp x3, x4, [x1, #16]
    ldp x3, x4, [x2, #32]
    stp x3, x4, [x1, #32]
    ldp x3, x4, [x2, #48]
    stp x3, x4, [x1, #48]
    ldr x3, [x2, #64]
    str x3, [x1, #64]
    ldr x3, [x2, #240]
    str x3, [x1, #72]
    ldr x3, [x2, #248]
    str x3, [x1, #80]
    ldr x3, [x2, #256]
    str x3, [x1, #88]
    ldr x3, [x2, #264]
    str w3, [x1, #96]
    // Registers we restore ourselves.
    ldr w3, [x2, #288]
    msr fpsr, x3
    ldr w3, [x2, #292]
    msr fpcr, x3
    add x3, x2, #304
    ldp q0, q1, [x3, #0]
    ldp q2, q3, [x3, #32]
    ldp q4, q5, [x3, #64]
    ldp q6, q7, [x3, #96]
    ldp q8, q9, [x3, #128]
    ldp q10, q11, [x3, #160]
    ldp q12, q13, [x3, #192]
    ldp q14, q15, [x3, #224]
    ldp q16, q17, [x3, #256]
    ldp q18, q19, [x3, #288]
    ldp q20, q21, [x3, #320]
    ldp q22, q23, [x3, #352]
    ldp q24, q25, [x3, #384]
    ldp q26, q27, [x3, #416]
    ldp q28, q29, [x3, #448]
    ldp q30, q31, [x3, #480]
    ldr x9, [x2, #72]
    ldp x10, x11, [x2, #80]
    ldp x12, x13, [x2, #96]
    ldp x14, x15, [x2, #112]
    ldp x16, x17, [x2, #128]
    ldp x18, x19, [x2, #144]
    ldp x20, x21, [x2, #160]
    ldp x22, x23, [x2, #176]
    ldp x24, x25, [x2, #192]
    ldp x26, x27, [x2, #208]
    ldp x28, x29, [x2, #224]
    mov w0, wzr
    svc 0x28
    b .

.Lxe_exc_unhandled:
    mov w0, #0xf801
    svc 0x28
    b .
.size __libnx_exception_entry, . - __libnx_exception_entry

.section .text.xe_switch_exception_trampoline, "ax", %progbits
.global xe_switch_exception_trampoline
.type xe_switch_exception_trampoline, %function
.align 4
xe_switch_exception_trampoline:
    // sp == x0 == frame. Returns only if the exception was handled.
    mov x19, x0
    mov x29, xzr
    bl xe_switch_dispatch_exception
    mov x0, x19
    )" XE_SWITCH_EXC_MAGIC_ASM("x1") R"(
    brk #)" XE_SWITCH_EXC_BRK_IMM R"(
    b .
.size xe_switch_exception_trampoline, . - xe_switch_exception_trampoline
.text
)");

namespace xe {

constexpr size_t kMaxHandlerCount = 8;

static std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

void ExceptionHandler::Install(Handler fn, void* data) {
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < xe::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }
}

namespace {

constexpr uint32_t kEsrEcShift = 26;
constexpr uint32_t kEcUnknown = 0x00;
constexpr uint32_t kEcInstructionAbortEl0 = 0x20;
constexpr uint32_t kEcDataAbortEl0 = 0x24;
constexpr uint32_t kEcBrk = 0x3C;

const char* DescribeEc(uint32_t ec) {
  switch (ec) {
    case kEcUnknown:
      return "undefined instruction";
    case kEcInstructionAbortEl0:
      return "instruction abort";
    case kEcDataAbortEl0:
      return "data abort";
    case 0x22:
      return "PC alignment fault";
    case 0x26:
      return "SP alignment fault";
    case kEcBrk:
      return "BRK";
    default:
      return "other";
  }
}

std::atomic<int> crash_in_progress{0};

[[noreturn]] void ReportFatalException(const XeSwitchExceptionFrame* f) {
  if (crash_in_progress.fetch_add(1) == 0) {
    uint32_t ec = f->esr >> kEsrEcShift;
    FILE* file = std::fopen("sdmc:/switch/xenia-nx/crash.txt", "w");
    char line[256];
    auto emit = [&](const char* text) {
      if (file) {
        std::fputs(text, file);
        std::fputc('\n', file);
      }
      XELOGE("{}", text);
    };
    std::snprintf(line, sizeof(line),
                  "Unhandled CPU exception: %s (type %u, esr 0x%08" PRIx32
                  ", far 0x%016" PRIx64 ")",
                  DescribeEc(ec), f->type, f->esr, f->far);
    emit(line);
    std::snprintf(line, sizeof(line),
                  "pc 0x%016" PRIx64 " lr 0x%016" PRIx64 " sp 0x%016" PRIx64
                  " pstate 0x%08" PRIx64,
                  f->pc, f->x[30], f->sp, f->pstate);
    emit(line);
    for (int i = 0; i < 30; i += 3) {
      std::snprintf(line, sizeof(line),
                    "x%-2d 0x%016" PRIx64 "  x%-2d 0x%016" PRIx64
                    "  x%-2d 0x%016" PRIx64,
                    i, f->x[i], i + 1, f->x[i + 1], i + 2, f->x[i + 2]);
      emit(line);
    }
    if (file) {
      std::fclose(file);
    }
    xe::FlushLog();
  }
  diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}

}  // namespace

}  // namespace xe

extern "C" void xe_switch_dispatch_exception(XeSwitchExceptionFrame* f) {
  using namespace xe;
  uint32_t ec = f->esr >> kEsrEcShift;

  // First touch of a reserved guest memory chunk.
  if (ec == kEcDataAbortEl0 && memory::HandleSwitchLazyFault(f->far)) {
    return;
  }

  HostThreadContext thread_context;
  std::memcpy(thread_context.x, f->x, sizeof(thread_context.x));
  thread_context.sp = f->sp;
  thread_context.pc = f->pc;
  thread_context.pstate = f->pstate;
  thread_context.fpsr = f->fpsr;
  thread_context.fpcr = f->fpcr;
  std::memcpy(thread_context.v, f->v, sizeof(thread_context.v));

  Exception ex;
  if (ec == kEcDataAbortEl0 || ec == kEcInstructionAbortEl0) {
    Exception::AccessViolationOperation operation;
    if (ec == kEcInstructionAbortEl0) {
      operation = Exception::AccessViolationOperation::kRead;
    } else {
      operation = (f->esr & (UINT32_C(1) << 6))
                      ? Exception::AccessViolationOperation::kWrite
                      : Exception::AccessViolationOperation::kRead;
    }
    ex.InitializeAccessViolation(&thread_context, f->far, operation);
  } else {
    ex.InitializeIllegalInstruction(&thread_context);
  }

  for (size_t i = 0; i < xe::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      uint32_t modified_register_index;
      uint32_t modified_x = ex.modified_x_registers();
      while (xe::bit_scan_forward(modified_x, &modified_register_index)) {
        modified_x &= ~(UINT32_C(1) << modified_register_index);
        f->x[modified_register_index] =
            thread_context.x[modified_register_index];
      }
      uint32_t modified_v = ex.modified_v_registers();
      while (xe::bit_scan_forward(modified_v, &modified_register_index)) {
        modified_v &= ~(UINT32_C(1) << modified_register_index);
        std::memcpy(f->v[modified_register_index],
                    &thread_context.v[modified_register_index],
                    sizeof(f->v[0]));
      }
      f->sp = thread_context.sp;
      f->pc = thread_context.pc;
      f->pstate = thread_context.pstate;
      f->fpsr = thread_context.fpsr;
      f->fpcr = thread_context.fpcr;
      return;
    }
  }

  ReportFatalException(f);
}
