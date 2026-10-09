/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Horizon (Nintendo Switch) threading, derived from threading_posix.cc:
// libnx provides pthreads, but there are no signals, futexes or robust
// mutexes; suspension uses svcSetThreadActivity and the multi-object wait
// uses the kernel address arbiter.

#include "xenia/base/threading.h"

#include "xenia/base/assert.h"
#include "xenia/base/chrono_steady_cast.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading_timer_queue.h"

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <switch.h>
#include <unistd.h>
#include <array>
#include <climits>
#include <cstddef>
#include <ctime>

#include "logging.h"

namespace xe {
namespace threading {

template <typename _Rep, typename _Period>
timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Horizon has no POSIX signals. Other threads are suspended with
// svcSetThreadActivity, and alertable waits are not interrupted.

namespace {
// Kernel thread handle of the calling thread.
Handle CurrentThreadKernelHandle() { return threadGetCurHandle(); }

uint64_t KernelThreadId(Handle handle) {
  u64 id = 0;
  svcGetThreadId(&id, handle);
  return id;
}

// The application may use cores 0-2 (core 3 belongs to the system). The
// Xbox 360 has 6 hardware threads; fold them onto the 3 cores.
constexpr uint32_t kSwitchUsableCoreMask = 0b111;
constexpr int kSwitchUsableCoreCount = 3;
}  // namespace

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(KernelThreadId(CurrentThreadKernelHandle()));
}

void MaybeYield() {
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() { __sync_synchronize(); }

static void SleepFor(timespec rqtp) {
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

void Sleep(std::chrono::microseconds duration) {
  SleepFor(DurationToTimeSpec(duration));
}

void NanoSleep(int64_t duration) {
  // Not Sleep(nanoseconds(duration)): the conversion to microseconds
  // truncates, so anything under 1000 ns became nanosleep(0).
  if (duration <= 0) {
    return;
  }
  SleepFor({static_cast<time_t>(duration / 1000000000LL),
            static_cast<long>(duration % 1000000000LL)});
}

void NanoSleepPrecise(int64_t ns) {
#if XE_PLATFORM_MAC
  // Darwin's nanosleep can oversleep by 100-500us under load. Land precisely
  // on the deadline by using mach_wait_until for the bulk of the wait and
  // busy-waiting the last ~200us.
  if (ns <= 0) {
    return;
  }
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    return i;
  }();
  constexpr uint64_t kSpinTailNs = 200'000;
  const uint64_t deadline =
      mach_absolute_time() +
      static_cast<uint64_t>((static_cast<__uint128_t>(ns) * tb.denom) /
                            tb.numer);
  const uint64_t spin_tail = static_cast<uint64_t>(
      (static_cast<__uint128_t>(kSpinTailNs) * tb.denom) / tb.numer);
  if (deadline > mach_absolute_time() + spin_tail) {
    mach_wait_until(deadline - spin_tail);
  }
  while (mach_absolute_time() < deadline) {
  }
#else
  NanoSleep(ns);
#endif
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  Sleep(duration);
  alertable_state_ = false;
  return SleepResult::kSuccess;
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) { return pthread_key_delete(handle) == 0; }

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(handle));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(handle, reinterpret_cast<void*>(value)) == 0;
}

// A multi-object wait blocks on this counter instead of polling. Every signal
// bumps it and, on Linux, wakes the waiters with a futex. Neither side takes a
// lock, so signalling stays safe from a user callback running in a signal
// handler. With no multi-object wait in progress, signalling is one load.
std::atomic<uint32_t> multi_wait_epoch{0};
std::atomic<uint32_t> multi_wait_waiters{0};

void NotifyMultiObjectWaiters() {
  if (multi_wait_waiters.load(std::memory_order_seq_cst) == 0) {
    return;
  }
  multi_wait_epoch.fetch_add(1, std::memory_order_seq_cst);
  svcSignalToAddress(reinterpret_cast<void*>(&multi_wait_epoch),
                     SignalType_Signal, 0, -1);
}

// Blocks until the epoch differs from `expected` or `timeout` passes.
void WaitForMultiObjectSignal(uint32_t expected,
                              std::chrono::milliseconds timeout) {
  svcWaitForAddress(reinterpret_cast<void*>(&multi_wait_epoch),
                    ArbitrationType_WaitIfEqual, s32(expected),
                    s64(timeout.count()) * 1000000);
}

class PosixConditionBase {
 public:
  PosixConditionBase() {
    // Horizon has no robust mutexes; threads are never killed while
    // holding these locks.
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    bool executed;
    auto predicate = [this] { return this->signaled(); };

    std::unique_lock<std::mutex> lock(mutex_);

    if (predicate()) {
      executed = true;
    } else {
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;  // Did not time out;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    if (executed) {
      post_execution();
      return WaitResult::kSuccess;
    }
    return WaitResult::kTimeout;
  }

  static std::pair<WaitResult, size_t> WaitMultiple(
      std::vector<PosixConditionBase*>&& handles, bool wait_all,
      std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    // For single handle, just use the normal Wait path
    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    // Check the handles in turn and block on the multi-object epoch between
    // checks.
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    // Registered before the first check, so a later signal bumps the epoch.
    multi_wait_waiters.fetch_add(1, std::memory_order_seq_cst);
    struct MultiWaitRegistration {
      ~MultiWaitRegistration() {
        multi_wait_waiters.fetch_sub(1, std::memory_order_seq_cst);
      }
    } multi_wait_registration;
    while (true) {
      // A signal while the handles are read changes the epoch, and the wait
      // below then returns at once.
      uint32_t epoch_before_check =
          multi_wait_epoch.load(std::memory_order_seq_cst);

      // Check all handles to see if any/all are signaled
      // Use try_lock to avoid deadlocks from lock ordering issues
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;

      // Try to acquire all locks without blocking
      std::vector<std::unique_lock<std::mutex>> locks;
      locks.reserve(handles.size());
      bool all_locked = true;

      for (size_t i = 0; i < handles.size(); ++i) {
        if (handles[i]->mutex_.try_lock()) {
          locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
        } else {
          // Couldn't acquire lock
          all_locked = false;
          break;
        }
      }

      // If we couldn't acquire all locks, release what we have and retry
      if (!all_locked) {
        locks.clear();
        std::this_thread::yield();
        continue;
      }

      // Now we have all locks, check the condition
      if (wait_all) {
        // For wait_all, check if ALL are signaled
        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;
      } else {
        // For wait_any, check if ANY is signaled
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        // Execute post_execution for the signaled handle(s)
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      // Release locks before sleeping
      locks.clear();

      // Check timeout
      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);
      WaitForMultiObjectSignal(
          epoch_before_check,
          std::min(remaining, std::chrono::milliseconds(50)));
    }
  }

  [[nodiscard]] virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  [[nodiscard]] inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;
  std::condition_variable cond_;
  std::mutex mutex_;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  ~PosixCondition() override = default;

  bool Signal() override {
    auto lock = std::unique_lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock(mutex_);
    signal_ = false;
  }

 private:
  [[nodiscard]] bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> final : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock(mutex_);
    // Validate that releasing would not exceed the maximum count
    if (count_ + release_count > maximum_count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
    return true;
  }

 private:
  [[nodiscard]] bool signaled() const override { return count_ > 0; }
  void post_execution() override {
    count_--;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> final : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        cond_.notify_all();
        NotifyMultiObjectWaiters();
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] void* native_handle() const override {
    return const_cast<std::mutex&>(mutex_).native_handle();
  }

 private:
  [[nodiscard]] bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> final : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  ~PosixCondition() override { Cancel(); }

  bool Signal() override {
    std::lock_guard lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time,
               std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period,
                    std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ =
        QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() const {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  [[nodiscard]] void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = static_cast<PosixCondition*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

  [[nodiscard]] bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> final : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
  }
  bool Initialize(Thread::CreationParameters params,
                  ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      return false;
    }
    if (params.stack_size &&
        pthread_attr_setstacksize(&attr, params.stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    // Scheduling priority is applied through set_priority() once the kernel
    // thread exists.
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        kernel_handle_(CurrentThreadKernelHandle()),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
    kernel_id_ = KernelThreadId(kernel_handle_);
    sem_init(&suspend_sem_, 0, 0);
  }

  ~PosixCondition() override = default;

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    std::lock_guard lock(name_mutex_);
    return name_;
  }

  void set_name(const std::string& name) const {
    WaitStarted();
    std::lock_guard lock(name_mutex_);
    name_ = name;
  }

  uint32_t system_id() const {
    WaitStarted();
    return static_cast<uint32_t>(kernel_id_);
  }

  uint64_t affinity_mask() const {
    WaitStarted();
    s32 preferred_core = 0;
    u64 core_mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&preferred_core, &core_mask,
                                      kernel_handle_))) {
      return kSwitchUsableCoreMask;
    }
    return core_mask & kSwitchUsableCoreMask;
  }

  void set_affinity_mask(uint64_t mask) const {
    WaitStarted();
    // Fold the requested (Xbox 360 hardware thread) mask onto cores 0-2.
    uint32_t core_mask = 0;
    for (int i = 0; i < 64; ++i) {
      if (mask & (uint64_t(1) << i)) {
        core_mask |= 1u << (i % kSwitchUsableCoreCount);
      }
    }
    core_mask &= kSwitchUsableCoreMask;
    if (!core_mask) {
      core_mask = kSwitchUsableCoreMask;
    }
    int preferred_core = __builtin_ctz(core_mask);
    svcSetThreadCoreMask(kernel_handle_, preferred_core, core_mask);
  }

  int priority() const {
    WaitStarted();
    return priority_;
  }

  void set_priority(int new_priority) const {
    WaitStarted();
    priority_ = new_priority;
    // Xenia uses a SCHED_FIFO-like scale (higher = more urgent, 16 = normal).
    // Horizon uses 0..63 (lower = more urgent); application threads normally
    // sit at 44. Stay within 40..50 so the system stays responsive.
    int hos_priority = 44 - (new_priority - 16) / 4;
    hos_priority = std::clamp(hos_priority, 40, 50);
    svcSetThreadPriority(kernel_handle_, hos_priority);
  }

  void QueueUserCallback(std::function<void()> callback) {
    // Only used to wake alertable waits (with an empty callback). Without
    // signals, alertable waits just run to completion.
    WaitStarted();
    std::unique_lock lock(callback_mutex_);
    user_callback_ = std::move(callback);
  }

  void CallUserCallback() const {
    std::unique_lock lock(callback_mutex_);
    if (user_callback_) {
      user_callback_();
    }
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      if (kernel_paused_) {
        kernel_paused_ = false;
        svcSetThreadActivity(kernel_handle_, ThreadActivity_Runnable);
      } else {
        sem_post(&suspend_sem_);
      }
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();

    bool is_current_thread = pthread_self() == thread_;
    bool already_suspended = false;
    {
      std::unique_lock lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      already_suspended = (state_ == State::kSuspended);
      state_ = State::kSuspended;
      ++suspend_count_;
      if (!already_suspended && !is_current_thread) {
        // The kernel stops the thread at its next scheduling point.
        Result rc =
            svcSetThreadActivity(kernel_handle_, ThreadActivity_Paused);
        kernel_paused_ = R_SUCCEEDED(rc);
        return kernel_paused_;
      }
    }
    if (already_suspended) {
      return true;
    }
    // Self-suspension.
    WaitSuspended();
    return true;
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          lock.unlock();
          pthread_exit(reinterpret_cast<void*>(intptr_t(exit_code)));
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard lock(mutex_);
      exit_code_ = exit_code;
      signaled_ = true;
      cond_.notify_all();
      NotifyMultiObjectWaiters();
    }
    if (is_current_thread) {
      pthread_exit(reinterpret_cast<void*>(intptr_t(exit_code)));
    }
    // Horizon can't kill a single thread of the current process; park it for
    // good instead. Its resources are reclaimed when the process exits.
    svcSetThreadActivity(kernel_handle_, ThreadActivity_Paused);
    terminated_externally_ = true;
  }

  void WaitStarted() const {
    std::unique_lock lock(state_mutex_);
    state_signal_.wait(lock,
                       [this] { return state_ != State::kUninitialized; });
  }

  /// Blocks the calling thread while its suspend count is non-zero.
  void WaitSuspended() {
    while (suspend_count_.load(std::memory_order_acquire) > 0) {
      int ret;
      do {
        ret = sem_wait(&suspend_sem_);
      } while (ret == -1 && errno == EINTR);
      if (ret == -1) {
        break;
      }
    }
  }

  void* native_handle() const override {
    return reinterpret_cast<void*>(thread_);
  }

 private:
  static void* ThreadStartRoutine(void* parameter);
  bool signaled() const override { return signaled_; }
  void post_execution() override {
    bool expected = false;
    if (thread_ && !terminated_externally_ &&
        joined_.compare_exchange_strong(expected, true)) {
      pthread_join(thread_, nullptr);
      sem_destroy(&suspend_sem_);
    }
  }
  pthread_t thread_;
  Handle kernel_handle_ = INVALID_HANDLE;
  uint64_t kernel_id_ = 0;
  bool signaled_;
  int exit_code_;
  State state_;  // Protected by state_mutex_
  std::atomic<uint32_t> suspend_count_;
  bool kernel_paused_ = false;  // Protected by state_mutex_
  bool terminated_externally_ = false;
  std::atomic<bool> joined_{false};  // Prevents a second pthread_join
  sem_t suspend_sem_;
  mutable int priority_ = 16;
  mutable std::mutex name_mutex_;
  mutable std::string name_;
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::function<void()> user_callback_;
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

// Out-of-line destructor to ensure proper RTTI/vtable emission
PosixWaitHandle::~PosixWaitHandle() = default;

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  [[nodiscard]] void* native_handle() const override {
    return handle_.native_handle();
  }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner)
    : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset)
    : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset,
                                                  bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread)
    : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable,
                std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  auto result = posix_wait_handle->condition().Wait(timeout);
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal,
                         WaitHandle* wait_handle_to_wait_on, bool is_alertable,
                         std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal =
      dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on =
      dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr ||
      posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  if (posix_wait_handle_to_signal->condition().Signal()) {
    result = posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[],
                                           size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  auto result = PosixConditionBase::WaitMultiple(std::move(conditions),
                                                 wait_all, timeout);
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

class PosixEvent final : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  EventInfo Query() override {
    EventInfo result{};
    assert_always();
    return result;
  }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore final : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count),
                           out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count,
                                             int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count ||
      maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant final : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner)
      : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer final : public PosixConditionHandle<Timer> {
  using WClock_ = WClock_;
  using GClock_ = GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(xe::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(date::clock_cast<GClock_>(due_time),
                     std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(
      xe::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(date::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread final : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params,
                  std::function<void()> start_routine) {
    auto start_data =
        new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override {
    handle_.set_affinity_mask(mask);
  }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override {
    handle_.set_priority(new_priority);
  }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  thread->handle_.kernel_handle_ = CurrentThreadKernelHandle();
  thread->handle_.kernel_id_ = KernelThreadId(thread->handle_.kernel_handle_);
  {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.state_ =
        create_suspended ? State::kSuspended : State::kRunning;
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.suspend_count_ = 1;
    thread->handle_.state_signal_.wait(
        lock, [thread] { return thread->handle_.suspend_count_ == 0; });
  }

  start_routine();

  {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.cond_.notify_all();
    NotifyMultiObjectWaiters();
  }

  current_thread_ = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine))) {
    return nullptr;
  }
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    pthread_exit(reinterpret_cast<void*>(intptr_t(exit_code)));
  }
  // Function must not return
  assert_always();
}

void set_name(const std::string_view name) {
  // Horizon thread names are fixed at creation; keep Xenia's copy only.
  if (current_thread_) {
    current_thread_->condition().set_name(std::string(name));
  }
}

}  // namespace threading
}  // namespace xe
