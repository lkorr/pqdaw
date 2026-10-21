#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include "pq/core/cache_line.h"

namespace pq {

// Fixed-size heap array aligned to a cache line (or to alignof(T) if larger), SPEC §13.
// The allocation is rounded up to whole cache lines so no other heap object shares its last line.
// Allocates in the constructor and frees in the destructor: create and destroy it off the audio thread.
// Element access is RT-safe.
template <class T>
class AlignedBuffer {
public:
  static constexpr std::size_t kAlignment = alignof(T) > kCacheLineSize ? alignof(T) : kCacheLineSize;

  AlignedBuffer() noexcept = default;

  // Value-initialises n elements (zero for arithmetic types).
  explicit AlignedBuffer(std::size_t n) : size_(n) {
    if (n == 0) return;
    void* raw = ::operator new(allocBytes(n), std::align_val_t{kAlignment});
    data_ = static_cast<T*>(raw);
    std::size_t built = 0;
    try {
      for (; built < n; ++built) ::new (static_cast<void*>(data_ + built)) T();
    } catch (...) {
      std::destroy_n(data_, built);
      ::operator delete(raw, allocBytes(n), std::align_val_t{kAlignment});
      throw;
    }
  }

  ~AlignedBuffer() { release(); }

  AlignedBuffer(AlignedBuffer&& o) noexcept
      : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}
  AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
    if (this != &o) {
      release();
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0);
    }
    return *this;
  }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  // RT-safe accessors.
  T* data() noexcept { return data_; }
  const T* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  T& operator[](std::size_t i) noexcept { assert(i < size_); return data_[i]; }
  const T& operator[](std::size_t i) const noexcept { assert(i < size_); return data_[i]; }
  T* begin() noexcept { return data_; }
  T* end() noexcept { return data_ + size_; }
  const T* begin() const noexcept { return data_; }
  const T* end() const noexcept { return data_ + size_; }
  std::span<T> span() noexcept { return {data_, size_}; }
  std::span<const T> span() const noexcept { return {data_, size_}; }

private:
  static std::size_t allocBytes(std::size_t n) noexcept {
    const std::size_t bytes = n * sizeof(T);
    return (bytes + kAlignment - 1) / kAlignment * kAlignment;
  }

  void release() noexcept {
    if (!data_) return;
    std::destroy_n(data_, size_);
    ::operator delete(static_cast<void*>(data_), allocBytes(size_), std::align_val_t{kAlignment});
    data_ = nullptr;
    size_ = 0;
  }

  T* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace pq
