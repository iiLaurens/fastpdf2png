// fastpdf2png - Thread-local memory pool
// SPDX-License-Identifier: MIT

#ifndef FASTPDF2PNG_MEMORY_POOL_H_
#define FASTPDF2PNG_MEMORY_POOL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef _WIN32
#include <malloc.h>
#define ALIGNED_ALLOC(align, size) _aligned_malloc(size, align)
#define ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
#include <sys/mman.h>
#define ALIGNED_ALLOC(align, size) aligned_alloc_wrapper(align, size)
#define ALIGNED_FREE(ptr) free(ptr)

inline void* aligned_alloc_wrapper(size_t alignment, size_t size) {
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0)
    return nullptr;
  return ptr;
}
#endif

#if defined(__linux__)
#include <sys/mman.h>
#define USE_HUGE_PAGES 1
#endif

namespace fast_png {

constexpr size_t kCacheLineSize = 64;
constexpr size_t kHugePageThreshold = 2 * 1024 * 1024;

class PageMemoryPool {
 public:
  PageMemoryPool() = default;
  ~PageMemoryPool() { Release(); }
  PageMemoryPool(const PageMemoryPool&) = delete;
  PageMemoryPool& operator=(const PageMemoryPool&) = delete;

  uint8_t* Acquire(size_t size) {
    if (size > capacity_) {
      Release();
      size_t new_capacity = size + size / 4;

#if USE_HUGE_PAGES
      if (new_capacity >= kHugePageThreshold) {
        buffer_ = static_cast<uint8_t*>(
            mmap(nullptr, new_capacity, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (buffer_ != MAP_FAILED) {
          madvise(buffer_, new_capacity, MADV_HUGEPAGE);
          capacity_ = new_capacity;
          use_mmap_ = true;
          return buffer_;
        }
        buffer_ = nullptr;
      }
#endif
      buffer_ = static_cast<uint8_t*>(ALIGNED_ALLOC(kCacheLineSize, new_capacity));
      if (buffer_) {
        capacity_ = new_capacity;
        use_mmap_ = false;
      }
    }
    return buffer_;
  }

 private:
  void Release() {
    if (!buffer_) return;
#if USE_HUGE_PAGES
    if (use_mmap_) munmap(buffer_, capacity_);
    else ALIGNED_FREE(buffer_);
#else
    ALIGNED_FREE(buffer_);
#endif
    buffer_ = nullptr;
    capacity_ = 0;
  }

  uint8_t* buffer_ = nullptr;
  size_t capacity_ = 0;
  bool use_mmap_ = false;
};

inline PageMemoryPool& GetThreadLocalPool() {
  static thread_local PageMemoryPool pool;
  return pool;
}

}  // namespace fast_png

#endif  // FASTPDF2PNG_MEMORY_POOL_H_
