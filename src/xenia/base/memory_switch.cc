/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Host memory management for Horizon (Nintendo Switch).
//
// Xenia expects an mmap-like OS: reserve address space at fixed addresses,
// back the same "file" at several virtual addresses (aliases), commit lazily,
// and have separate RX / RW views of the JIT code. Horizon has none of that
// for homebrew directly, but its kernel lets a process:
//
//   - svcMapProcessCodeMemory:   move heap pages to another address
//                                (state AliasCode, can be made RW or RX with
//                                svcSetProcessMemoryPermission);
//   - svcMapProcessMemory:       map those pages again elsewhere (state
//                                SharedCode, always RW);
//   - svcSetMemoryPermission:    reprotect AliasCodeData pages (R/RW/none).
//
// So an emulated "file mapping" here is a list of heap-backed chunks. Each
// chunk has one *primary* mapping (AliasCode) and is aliased into every view
// with svcMapProcessMemory. Chunks are backed on commit, or on first touch
// through the exception handler (see HandleSwitchLazyFault), which mirrors
// what Linux does with a sparse memfd.
//
// Limitations compared to POSIX:
//   - Alias (SharedCode) views are always RW; Protect() on them is recorded
//     but not enforced. Only primary mappings can really be reprotected.
//   - No RWX memory: executable mappings get a separate RW alias, which W^X
//     aware callers obtain through GetSwitchWritableAlias().

#include "xenia/base/memory.h"

#include <switch.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory_switch.h"
#include "xenia/base/platform.h"

namespace xe {
namespace memory {

namespace {

constexpr size_t kHostPageSize = 0x1000;
// Backing granularity. Large enough to keep kernel memory block counts sane,
// small enough that lazily touched sparse ranges don't waste much.
constexpr size_t kChunkSize = 0x10000;

inline uintptr_t PageAlignDown(uintptr_t v) { return v & ~(kHostPageSize - 1); }
inline uintptr_t PageAlignUp(uintptr_t v) {
  return (v + kHostPageSize - 1) & ~(kHostPageSize - 1);
}

bool IsExecutable(PageAccess access) {
  return access == PageAccess::kExecuteReadOnly ||
         access == PageAccess::kExecuteReadWrite;
}

u32 ToHorizonPerm(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return Perm_None;
    case PageAccess::kReadOnly:
      return Perm_R;
    case PageAccess::kReadWrite:
      return Perm_Rw;
    case PageAccess::kExecuteReadOnly:
    case PageAccess::kExecuteReadWrite:
      return Perm_Rx;
    default:
      return Perm_Rw;
  }
}

struct Region {
  uintptr_t start = 0;
  uintptr_t end = 0;
  bool Overlaps(uintptr_t s, uintptr_t e) const {
    return start != end && s < end && start < e;
  }
};

struct AddressSpaceInfo {
  Region aslr;
  Region heap;
  Region alias;
  Region stack;
  bool valid = false;
};

const AddressSpaceInfo& GetAddressSpaceInfo() {
  static AddressSpaceInfo info = [] {
    AddressSpaceInfo r;
    u64 base = 0, size = 0;
    auto get = [](Region& region, InfoType addr_id, InfoType size_id) {
      u64 b = 0, s = 0;
      if (R_SUCCEEDED(svcGetInfo(&b, addr_id, CUR_PROCESS_HANDLE, 0)) &&
          R_SUCCEEDED(svcGetInfo(&s, size_id, CUR_PROCESS_HANDLE, 0))) {
        region.start = b;
        region.end = b + s;
        return true;
      }
      return false;
    };
    (void)base;
    (void)size;
    r.valid = get(r.aslr, InfoType_AslrRegionAddress, InfoType_AslrRegionSize);
    get(r.heap, InfoType_HeapRegionAddress, InfoType_HeapRegionSize);
    get(r.alias, InfoType_AliasRegionAddress, InfoType_AliasRegionSize);
    get(r.stack, InfoType_StackRegionAddress, InfoType_StackRegionSize);
    return r;
  }();
  return info;
}

struct Mapping;

// A contiguous piece of host address space owned by xe::memory.
struct View {
  Mapping* mapping = nullptr;
  uintptr_t base = 0;
  size_t length = 0;
  uint64_t file_offset = 0;
  PageAccess access = PageAccess::kNoAccess;
  // Primary views receive the AliasCode mapping of chunks; all other views
  // get SharedCode aliases.
  bool primary = false;
  // Hidden views are internal (mirror / RW alias of executable memory).
  bool hidden = false;
  VirtmemReservation* reservation = nullptr;

  uintptr_t end() const { return base + length; }
  bool CoversOffset(uint64_t offset) const {
    return offset >= file_offset && offset < file_offset + length;
  }
  uintptr_t AddressOfOffset(uint64_t offset) const {
    return base + uintptr_t(offset - file_offset);
  }
};

// One heap allocation backing a run of consecutive chunks.
struct BackingRun {
  void* heap = nullptr;
  uint64_t offset = 0;  // File offset of the first byte.
  size_t size = 0;
  uint32_t chunks_alive = 0;
};

struct Mapping {
  size_t size = 0;
  bool executable = false;
  // Index = chunk number; null = not backed.
  std::vector<BackingRun*> chunk_runs;
  std::vector<View*> views;  // Insertion order.
  View* primary = nullptr;
  // RW alias for executable mappings (W^X).
  View* write_alias = nullptr;
  // If set, this mapping is owned by a single AllocFixed() call.
  bool anonymous = false;
};

struct State {
  std::mutex mutex;
  // Views keyed by base address, for address lookups.
  std::map<uintptr_t, View*> views_by_address;
  // File mapping handle table (index = handle - 1).
  std::vector<Mapping*> handles;
  bool logged_unenforced_protect = false;
  bool logged_capabilities = false;
};

State& state() {
  static State* s = new State();
  return *s;
}

Handle SelfProcess() { return envGetOwnProcessHandle(); }

void LogCapabilitiesOnce() {
  auto& st = state();
  if (st.logged_capabilities) {
    return;
  }
  st.logged_capabilities = true;
  const auto& as = GetAddressSpaceInfo();
  XELOGI(
      "Horizon memory: ASLR {:X}-{:X}, heap {:X}-{:X}, alias {:X}-{:X}, "
      "stack {:X}-{:X}",
      as.aslr.start, as.aslr.end, as.heap.start, as.heap.end, as.alias.start,
      as.alias.end, as.stack.start, as.stack.end);
  XELOGI(
      "Horizon memory: syscalls MapProcessCodeMemory={} "
      "SetProcessMemoryPermission={} MapProcessMemory={} "
      "SetMemoryPermission={}, own process handle {:X}",
      envIsSyscallHinted(0x77), envIsSyscallHinted(0x73),
      envIsSyscallHinted(0x74), envIsSyscallHinted(0x02), SelfProcess());
}

View* FindViewLocked(uintptr_t address) {
  auto& st = state();
  auto it = st.views_by_address.upper_bound(address);
  if (it == st.views_by_address.begin()) {
    return nullptr;
  }
  --it;
  View* view = it->second;
  if (address >= view->base && address < view->end()) {
    return view;
  }
  return nullptr;
}

bool RangeOverlapsOurViewsLocked(uintptr_t start, uintptr_t end) {
  auto& st = state();
  auto it = st.views_by_address.lower_bound(start);
  if (it != st.views_by_address.begin()) {
    auto prev = std::prev(it);
    if (prev->second->end() > start) {
      return true;
    }
  }
  return it != st.views_by_address.end() && it->first < end;
}

// Whether [start, end) can host AliasCode/SharedCode mappings and nothing is
// there yet.
bool IsRangeAvailableLocked(uintptr_t start, uintptr_t end) {
  const auto& as = GetAddressSpaceInfo();
  if (!as.valid || start >= end) {
    return false;
  }
  if (start < as.aslr.start || end > as.aslr.end) {
    return false;
  }
  if (as.heap.Overlaps(start, end) || as.alias.Overlaps(start, end) ||
      as.stack.Overlaps(start, end)) {
    return false;
  }
  if (RangeOverlapsOurViewsLocked(start, end)) {
    return false;
  }
  uintptr_t cursor = start;
  while (cursor < end) {
    MemoryInfo info = {};
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&info, &page_info, cursor))) {
      return false;
    }
    if (info.type != MemType_Unmapped) {
      return false;
    }
    uintptr_t next = uintptr_t(info.addr + info.size);
    if (next <= cursor) {
      return false;
    }
    cursor = next;
  }
  return true;
}

View* CreateViewLocked(Mapping* mapping, uintptr_t base, size_t length,
                       uint64_t file_offset, PageAccess access, bool hidden) {
  auto view = new View();
  view->mapping = mapping;
  view->base = base;
  view->length = length;
  view->file_offset = file_offset;
  view->access = access;
  view->hidden = hidden;
  virtmemLock();
  view->reservation = virtmemAddReservation(reinterpret_cast<void*>(base),
                                            length);
  virtmemUnlock();
  mapping->views.push_back(view);
  state().views_by_address[base] = view;
  return view;
}

// Finds free address space for a view. Fixed-address requests are validated.
uintptr_t PickAddressLocked(void* requested, size_t length) {
  if (requested) {
    uintptr_t base = reinterpret_cast<uintptr_t>(requested);
    if ((base & (kHostPageSize - 1)) ||
        !IsRangeAvailableLocked(base, base + length)) {
      return 0;
    }
    return base;
  }
  virtmemLock();
  void* found = virtmemFindAslr(length, kHostPageSize);
  virtmemUnlock();
  if (!found) {
    return 0;
  }
  uintptr_t base = reinterpret_cast<uintptr_t>(found);
  if (RangeOverlapsOurViewsLocked(base, base + length)) {
    // Our reservations are registered with virtmem, so this should not
    // happen, but be defensive.
    return 0;
  }
  return base;
}

// Maps (part of) a backing run into one non-primary view.
bool MapRunIntoAliasViewLocked(const BackingRun& run, uintptr_t primary_base,
                               View* view) {
  uint64_t lo = std::max<uint64_t>(run.offset, view->file_offset);
  uint64_t hi = std::min<uint64_t>(run.offset + run.size,
                                   view->file_offset + view->length);
  if (lo >= hi) {
    return true;
  }
  uintptr_t dst = view->AddressOfOffset(lo);
  uintptr_t src = primary_base + uintptr_t(lo - run.offset);
  Result rc = svcMapProcessMemory(reinterpret_cast<void*>(dst), SelfProcess(),
                                  src, hi - lo);
  if (R_FAILED(rc)) {
    XELOGE("svcMapProcessMemory({:X} <- {:X}, {:X}) failed: {:X}", dst, src,
           hi - lo, rc);
    return false;
  }
  return true;
}

void UnmapRunFromAliasViewLocked(const BackingRun& run, uintptr_t primary_base,
                                 View* view) {
  uint64_t lo = std::max<uint64_t>(run.offset, view->file_offset);
  uint64_t hi = std::min<uint64_t>(run.offset + run.size,
                                   view->file_offset + view->length);
  if (lo >= hi) {
    return;
  }
  uintptr_t dst = view->AddressOfOffset(lo);
  uintptr_t src = primary_base + uintptr_t(lo - run.offset);
  svcUnmapProcessMemory(reinterpret_cast<void*>(dst), SelfProcess(), src,
                        hi - lo);
}

// Backs file range [offset, offset + size) (chunk aligned, all unbacked) with
// one heap allocation.
bool CreateRunLocked(Mapping* mapping, uint64_t offset, size_t size) {
  View* primary = mapping->primary;
  if (!primary || !primary->CoversOffset(offset) ||
      !primary->CoversOffset(offset + size - 1)) {
    XELOGE("Horizon memory: no primary view for offset {:X}", offset);
    return false;
  }
  void* heap = aligned_alloc(kHostPageSize, size);
  if (!heap) {
    XELOGE("Horizon memory: out of memory backing {:X} bytes", size);
    return false;
  }
  std::memset(heap, 0, size);

  uintptr_t primary_addr = primary->AddressOfOffset(offset);
  Result rc = svcMapProcessCodeMemory(SelfProcess(), primary_addr,
                                      reinterpret_cast<u64>(heap), size);
  if (R_FAILED(rc)) {
    XELOGE("svcMapProcessCodeMemory({:X}, {:X}) failed: {:X}", primary_addr,
           size, rc);
    std::free(heap);
    return false;
  }
  u32 perm = mapping->executable ? Perm_Rx : Perm_Rw;
  rc = svcSetProcessMemoryPermission(SelfProcess(), primary_addr, size, perm);
  if (R_FAILED(rc)) {
    XELOGE("svcSetProcessMemoryPermission({:X}, {:X}, {}) failed: {:X}",
           primary_addr, size, perm, rc);
    svcUnmapProcessCodeMemory(SelfProcess(), primary_addr,
                              reinterpret_cast<u64>(heap), size);
    std::free(heap);
    return false;
  }

  auto run = new BackingRun();
  run->heap = heap;
  run->offset = offset;
  run->size = size;
  run->chunks_alive = uint32_t(size / kChunkSize);

  uintptr_t primary_base = primary->AddressOfOffset(offset);
  for (View* view : mapping->views) {
    if (view == primary) {
      continue;
    }
    MapRunIntoAliasViewLocked(*run, primary_base, view);
  }
  size_t first_chunk = size_t(offset / kChunkSize);
  for (size_t i = 0; i < run->chunks_alive; ++i) {
    mapping->chunk_runs[first_chunk + i] = run;
  }
  return true;
}

void DestroyRunLocked(Mapping* mapping, BackingRun* run) {
  View* primary = mapping->primary;
  uintptr_t primary_base = primary->AddressOfOffset(run->offset);
  for (View* view : mapping->views) {
    if (view != primary) {
      UnmapRunFromAliasViewLocked(*run, primary_base, view);
    }
  }
  // Code memory must be RW (data) or the original permission to unmap; reset
  // to RW in case it was reprotected.
  if (!mapping->executable) {
    svcSetMemoryPermission(reinterpret_cast<void*>(primary_base), run->size,
                           Perm_Rw);
  }
  svcUnmapProcessCodeMemory(SelfProcess(), primary_base,
                            reinterpret_cast<u64>(run->heap), run->size);
  std::free(run->heap);
  size_t first_chunk = size_t(run->offset / kChunkSize);
  for (size_t i = 0; i < run->size / kChunkSize; ++i) {
    mapping->chunk_runs[first_chunk + i] = nullptr;
  }
  delete run;
}

// Ensures every chunk overlapping file range [offset, offset + size) is
// backed, coalescing unbacked chunks into as few heap runs as possible.
bool EnsureBackedLocked(Mapping* mapping, uint64_t offset, size_t size) {
  if (!size) {
    return true;
  }
  size_t first = size_t(offset / kChunkSize);
  size_t last = size_t((offset + size - 1) / kChunkSize);
  last = std::min(last, mapping->chunk_runs.size() - 1);
  size_t i = first;
  while (i <= last) {
    if (mapping->chunk_runs[i]) {
      ++i;
      continue;
    }
    size_t run_end = i;
    while (run_end + 1 <= last && !mapping->chunk_runs[run_end + 1]) {
      ++run_end;
    }
    if (!CreateRunLocked(mapping, uint64_t(i) * kChunkSize,
                         (run_end - i + 1) * kChunkSize)) {
      return false;
    }
    i = run_end + 1;
  }
  return true;
}

// Releases runs fully inside file range [offset, offset + size); zeroes the
// remaining partially covered bytes.
void DecommitLocked(Mapping* mapping, uint64_t offset, size_t size) {
  if (!size) {
    return;
  }
  uint64_t end = offset + size;
  size_t first = size_t(offset / kChunkSize);
  size_t last = std::min(size_t((end - 1) / kChunkSize),
                         mapping->chunk_runs.size() - 1);
  for (size_t i = first; i <= last;) {
    BackingRun* run = mapping->chunk_runs[i];
    if (!run) {
      ++i;
      continue;
    }
    size_t run_last = size_t((run->offset + run->size) / kChunkSize) - 1;
    if (run->offset >= offset && run->offset + run->size <= end) {
      DestroyRunLocked(mapping, run);
    } else if (!mapping->executable) {
      // Partial: zero through the primary mapping (always RW-able).
      uint64_t lo = std::max<uint64_t>(run->offset, offset);
      uint64_t hi = std::min<uint64_t>(run->offset + run->size, end);
      uintptr_t addr = mapping->primary->AddressOfOffset(lo);
      svcSetMemoryPermission(reinterpret_cast<void*>(PageAlignDown(addr)),
                             PageAlignUp(addr + (hi - lo)) - PageAlignDown(addr),
                             Perm_Rw);
      std::memset(reinterpret_cast<void*>(addr), 0, size_t(hi - lo));
    }
    i = run_last + 1;
  }
}

void DestroyMappingLocked(Mapping* mapping) {
  // Destroy backing first (needs views for unmapping).
  for (size_t i = 0; i < mapping->chunk_runs.size(); ++i) {
    if (mapping->chunk_runs[i]) {
      DestroyRunLocked(mapping, mapping->chunk_runs[i]);
    }
  }
  auto& st = state();
  for (View* view : mapping->views) {
    st.views_by_address.erase(view->base);
    if (view->reservation) {
      virtmemLock();
      virtmemRemoveReservation(view->reservation);
      virtmemUnlock();
    }
    delete view;
  }
  mapping->views.clear();
  delete mapping;
}

Mapping* CreateMappingLocked(size_t length, bool executable) {
  auto mapping = new Mapping();
  mapping->size = xe::round_up(length, kChunkSize);
  mapping->executable = executable;
  mapping->chunk_runs.resize(mapping->size / kChunkSize, nullptr);
  return mapping;
}

// Adds a hidden view that receives the AliasCode mapping for data mappings,
// so every user-visible view can be a symmetric alias.
bool AddHiddenPrimaryLocked(Mapping* mapping) {
  uintptr_t base = PickAddressLocked(nullptr, mapping->size);
  if (!base) {
    XELOGE("Horizon memory: no address space for a {:X} byte mirror",
           mapping->size);
    return false;
  }
  View* view = CreateViewLocked(mapping, base, mapping->size, 0,
                                PageAccess::kReadWrite, true);
  view->primary = true;
  mapping->primary = view;
  return true;
}

Mapping* MappingFromHandleLocked(FileMappingHandle handle) {
  auto& st = state();
  if (handle <= 0 || size_t(handle) > st.handles.size()) {
    return nullptr;
  }
  return st.handles[handle - 1];
}

}  // namespace

size_t page_size() { return kHostPageSize; }
size_t allocation_granularity() { return kHostPageSize; }

// Horizon enforces W^X.
bool IsWritableExecutableMemorySupported() { return false; }

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  if (!length) {
    return nullptr;
  }
  auto& st = state();
  std::lock_guard lock(st.mutex);
  LogCapabilitiesOnce();

  const uintptr_t req = reinterpret_cast<uintptr_t>(base_address);
  const bool commit = allocation_type == AllocationType::kCommit ||
                      allocation_type == AllocationType::kReserveCommit;

  // Commit / protect inside something we already manage.
  if (base_address) {
    if (View* view = FindViewLocked(req)) {
      if (allocation_type == AllocationType::kReserve) {
        return nullptr;  // Already reserved.
      }
      size_t len = std::min<size_t>(length, view->end() - req);
      uint64_t offset = view->file_offset + (req - view->base);
      if (!EnsureBackedLocked(view->mapping, offset, len)) {
        return nullptr;
      }
      if (view->primary && !view->mapping->executable) {
        svcSetMemoryPermission(
            reinterpret_cast<void*>(PageAlignDown(req)),
            PageAlignUp(req + len) - PageAlignDown(req),
            ToHorizonPerm(access == PageAccess::kExecuteReadWrite
                              ? PageAccess::kReadWrite
                              : access));
      }
      return base_address;
    }
  }

  // New anonymous region.
  size_t aligned_length = PageAlignUp(length);
  uintptr_t base = PickAddressLocked(base_address, aligned_length);
  if (!base) {
    return nullptr;
  }
  const bool executable = IsExecutable(access);
  Mapping* mapping = CreateMappingLocked(aligned_length, executable);
  mapping->anonymous = true;
  View* view =
      CreateViewLocked(mapping, base, aligned_length, 0, access, false);
  view->primary = true;
  mapping->primary = view;
  if (executable) {
    // RW alias for W^X writers.
    uintptr_t alias_base = PickAddressLocked(nullptr, aligned_length);
    if (!alias_base) {
      DestroyMappingLocked(mapping);
      return nullptr;
    }
    mapping->write_alias = CreateViewLocked(mapping, alias_base, aligned_length,
                                            0, PageAccess::kReadWrite, true);
  }
  if (commit) {
    if (!EnsureBackedLocked(mapping, 0, aligned_length)) {
      DestroyMappingLocked(mapping);
      return nullptr;
    }
    if (!executable && access != PageAccess::kReadWrite) {
      svcSetMemoryPermission(reinterpret_cast<void*>(base), aligned_length,
                             ToHorizonPerm(access));
    }
  }
  return reinterpret_cast<void*>(base);
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  auto& st = state();
  std::lock_guard lock(st.mutex);
  uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  View* view = FindViewLocked(addr);
  if (!view) {
    return false;
  }
  Mapping* mapping = view->mapping;
  switch (deallocation_type) {
    case DeallocationType::kRelease:
      if (mapping->anonymous && view->base == addr) {
        DestroyMappingLocked(mapping);
        return true;
      }
      // Views of file mappings are released with UnmapFileView.
      return false;
    case DeallocationType::kDecommit: {
      size_t len = std::min<size_t>(length, view->end() - addr);
      DecommitLocked(mapping, view->file_offset + (addr - view->base), len);
      return true;
    }
    default:
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  auto& st = state();
  std::lock_guard lock(st.mutex);
  uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  View* view = FindViewLocked(addr);
  if (!view) {
    return false;
  }
  if (out_old_access) {
    *out_old_access = view->access;
  }
  size_t len = std::min<size_t>(length, view->end() - addr);
  Mapping* mapping = view->mapping;
  if (access != PageAccess::kNoAccess) {
    // Protecting implies committed memory.
    if (!EnsureBackedLocked(mapping, view->file_offset + (addr - view->base),
                            len)) {
      return false;
    }
  }
  if (mapping->executable) {
    // Executable memory stays RX; writes go through the RW alias.
    return true;
  }
  if (view->primary) {
    uintptr_t lo = PageAlignDown(addr);
    uintptr_t hi = PageAlignUp(addr + len);
    // Only backed pages can be reprotected; walk the chunks.
    bool ok = true;
    for (uintptr_t page = lo; page < hi;) {
      uint64_t offset = view->file_offset + (page - view->base);
      size_t chunk = size_t(offset / kChunkSize);
      uintptr_t chunk_end =
          std::min<uintptr_t>(hi, view->AddressOfOffset(
                                      uint64_t(chunk + 1) * kChunkSize));
      if (mapping->chunk_runs[chunk]) {
        Result rc = svcSetMemoryPermission(reinterpret_cast<void*>(page),
                                           chunk_end - page,
                                           ToHorizonPerm(access));
        ok = ok && R_SUCCEEDED(rc);
      }
      page = chunk_end;
    }
    return ok;
  }
  if (!st.logged_unenforced_protect && access != PageAccess::kReadWrite) {
    st.logged_unenforced_protect = true;
    XELOGW(
        "Horizon memory: protection on aliased views is not enforced "
        "(first at {:X})",
        addr);
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  auto& st = state();
  std::lock_guard lock(st.mutex);
  uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  View* view = FindViewLocked(addr);
  if (!view) {
    access_out = PageAccess::kNoAccess;
    length = 0;
    return false;
  }
  if (view->primary && !view->mapping->executable) {
    MemoryInfo info = {};
    u32 page_info = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, addr)) &&
        info.type != MemType_Unmapped) {
      length = size_t(std::min<uint64_t>(info.addr + info.size, view->end()) -
                      addr);
      switch (info.perm & (Perm_R | Perm_W | Perm_X)) {
        case Perm_R:
          access_out = PageAccess::kReadOnly;
          break;
        case Perm_Rw:
          access_out = PageAccess::kReadWrite;
          break;
        case Perm_Rx:
          access_out = PageAccess::kExecuteReadOnly;
          break;
        default:
          access_out = PageAccess::kNoAccess;
          break;
      }
      return true;
    }
  }
  length = view->end() - addr;
  access_out = view->access;
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
  (void)path;
  auto& st = state();
  std::lock_guard lock(st.mutex);
  LogCapabilitiesOnce();
  Mapping* mapping = CreateMappingLocked(length, IsExecutable(access));
  if (!mapping->executable) {
    if (!AddHiddenPrimaryLocked(mapping)) {
      DestroyMappingLocked(mapping);
      return kFileMappingHandleInvalid;
    }
    if (commit && !EnsureBackedLocked(mapping, 0, mapping->size)) {
      DestroyMappingLocked(mapping);
      return kFileMappingHandleInvalid;
    }
  }
  st.handles.push_back(mapping);
  return FileMappingHandle(st.handles.size());
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  (void)path;
  auto& st = state();
  std::lock_guard lock(st.mutex);
  Mapping* mapping = MappingFromHandleLocked(handle);
  if (!mapping) {
    return;
  }
  st.handles[handle - 1] = nullptr;
  DestroyMappingLocked(mapping);
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  auto& st = state();
  std::lock_guard lock(st.mutex);
  Mapping* mapping = MappingFromHandleLocked(handle);
  if (!mapping || !length || (file_offset & (kHostPageSize - 1)) ||
      file_offset + length > mapping->size) {
    return nullptr;
  }
  size_t aligned_length = PageAlignUp(length);
  uintptr_t base = PickAddressLocked(base_address, aligned_length);
  if (!base) {
    return nullptr;
  }
  View* view = CreateViewLocked(mapping, base, aligned_length, file_offset,
                                access, false);
  if (mapping->executable && !mapping->primary) {
    // The first view of executable memory holds the code (AliasCode, RX);
    // later views are RW aliases.
    if (file_offset != 0 || aligned_length < mapping->size) {
      XELOGE(
          "Horizon memory: the first view of executable memory must cover "
          "all of it");
    }
    view->primary = true;
    mapping->primary = view;
    return reinterpret_cast<void*>(base);
  }
  // Alias every already-backed run into the new view.
  BackingRun* last_run = nullptr;
  for (BackingRun* run : mapping->chunk_runs) {
    if (!run || run == last_run) {
      continue;
    }
    last_run = run;
    MapRunIntoAliasViewLocked(*run, mapping->primary->AddressOfOffset(
                                        run->offset),
                              view);
  }
  if (mapping->executable && !mapping->write_alias &&
      access == PageAccess::kReadWrite) {
    mapping->write_alias = view;
  }
  return reinterpret_cast<void*>(base);
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  (void)length;
  auto& st = state();
  std::lock_guard lock(st.mutex);
  Mapping* mapping = MappingFromHandleLocked(handle);
  uintptr_t base = reinterpret_cast<uintptr_t>(base_address);
  auto it = st.views_by_address.find(base);
  if (!mapping || it == st.views_by_address.end() ||
      it->second->mapping != mapping) {
    return false;
  }
  View* view = it->second;
  if (view->primary) {
    // Executable primary: drop all backing (aliases are unmapped with it).
    for (size_t i = 0; i < mapping->chunk_runs.size(); ++i) {
      if (mapping->chunk_runs[i]) {
        DestroyRunLocked(mapping, mapping->chunk_runs[i]);
      }
    }
    mapping->primary = nullptr;
  } else {
    BackingRun* last_run = nullptr;
    for (BackingRun* run : mapping->chunk_runs) {
      if (!run || run == last_run) {
        continue;
      }
      last_run = run;
      UnmapRunFromAliasViewLocked(
          *run, mapping->primary->AddressOfOffset(run->offset), view);
    }
  }
  if (mapping->write_alias == view) {
    mapping->write_alias = nullptr;
  }
  mapping->views.erase(
      std::find(mapping->views.begin(), mapping->views.end(), view));
  st.views_by_address.erase(it);
  if (view->reservation) {
    virtmemLock();
    virtmemRemoveReservation(view->reservation);
    virtmemUnlock();
  }
  delete view;
  return true;
}

// Switch-specific extensions (memory_switch.h).

void* GetSwitchWritableAlias(void* address) {
  auto& st = state();
  std::lock_guard lock(st.mutex);
  uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  View* view = FindViewLocked(addr);
  if (!view) {
    return nullptr;
  }
  Mapping* mapping = view->mapping;
  if (!mapping->executable) {
    return address;
  }
  View* alias = mapping->write_alias;
  if (!alias) {
    return nullptr;
  }
  uint64_t offset = view->file_offset + (addr - view->base);
  if (!alias->CoversOffset(offset)) {
    return nullptr;
  }
  return reinterpret_cast<void*>(alias->AddressOfOffset(offset));
}

bool HandleSwitchLazyFault(uintptr_t fault_address) {
  auto& st = state();
  std::lock_guard lock(st.mutex);
  View* view = FindViewLocked(fault_address);
  if (!view) {
    return false;
  }
  Mapping* mapping = view->mapping;
  uint64_t offset = view->file_offset + (fault_address - view->base);
  size_t chunk = size_t(offset / kChunkSize);
  if (chunk >= mapping->chunk_runs.size() || mapping->chunk_runs[chunk]) {
    // Already backed: a real protection fault.
    return false;
  }
  return EnsureBackedLocked(mapping, uint64_t(chunk) * kChunkSize, kChunkSize);
}

}  // namespace memory
}  // namespace xe
