/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia NX contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Horizon has no file-backed mmap. MappedMemory is emulated by reading the
// requested range into a heap buffer and, for writable mappings, writing it
// back on Flush() / Close(). Fine for small files (XEX, patches, saves); large
// disc images need a streaming device instead.

#include "xenia/base/mapped_memory.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

namespace xe {

class SwitchMappedMemory : public MappedMemory {
 public:
  SwitchMappedMemory(void* data, size_t size, FILE* file, size_t offset,
                     bool writable)
      : MappedMemory(data, size),
        file_(file),
        offset_(offset),
        writable_(writable) {}

  ~SwitchMappedMemory() override { Close(0); }

  static std::unique_ptr<SwitchMappedMemory> Open(
      const std::filesystem::path& path, Mode mode, size_t offset,
      size_t length) {
    const bool writable = mode == Mode::kReadWrite;
    FILE* file = std::fopen(path.c_str(), writable ? "r+b" : "rb");
    if (!file) {
      return nullptr;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
      std::fclose(file);
      return nullptr;
    }
    long file_size = std::ftell(file);
    if (file_size < 0 || size_t(file_size) < offset) {
      std::fclose(file);
      return nullptr;
    }
    size_t map_length = length ? length : size_t(file_size) - offset;
    if (!map_length) {
      std::fclose(file);
      return nullptr;
    }
    void* data = std::malloc(map_length);
    if (!data) {
      XELOGE("MappedMemory: out of memory mapping {} ({} bytes)",
             xe::path_to_utf8(path), map_length);
      std::fclose(file);
      return nullptr;
    }
    std::memset(data, 0, map_length);
    size_t available = std::min(map_length, size_t(file_size) - offset);
    std::fseek(file, long(offset), SEEK_SET);
    if (std::fread(data, 1, available, file) != available) {
      std::free(data);
      std::fclose(file);
      return nullptr;
    }
    return std::make_unique<SwitchMappedMemory>(data, map_length, file, offset,
                                                writable);
  }

  void Close(uint64_t truncate_size) override {
    if (data_) {
      Flush();
      std::free(data_);
      data_ = nullptr;
    }
    if (file_) {
      if (truncate_size && writable_) {
        std::fflush(file_);
        ftruncate(fileno(file_), off_t(truncate_size));
      }
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  void Flush() override {
    if (!writable_ || !data_ || !file_) {
      return;
    }
    std::fseek(file_, long(offset_), SEEK_SET);
    std::fwrite(data_, 1, size(), file_);
    std::fflush(file_);
  }

 private:
  FILE* file_;
  size_t offset_;
  bool writable_;
};

std::unique_ptr<MappedMemory> MappedMemory::Open(
    const std::filesystem::path& path, Mode mode, size_t offset,
    size_t length) {
  return SwitchMappedMemory::Open(path, mode, offset, length);
}

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size,
    bool low_address_space) {
  return nullptr;
}

}  // namespace xe
