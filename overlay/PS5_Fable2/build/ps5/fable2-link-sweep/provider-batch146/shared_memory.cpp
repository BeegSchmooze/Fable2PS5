#include <immintrin.h>
#include <vector>
#include <mutex>

#include <atomic>
namespace {
// Writers remain serialized by the existing global critical region.
// Only page-validity writes advance this epoch; all flag accesses below use
// native atomics so readers never race a non-atomic store.
static std::atomic<unsigned long long> page_epoch39{0};
static std::mutex page_mutex42;
struct PageWrite39 {
PageWrite39(){page_mutex42.lock();page_epoch39.fetch_add(1,std::memory_order_seq_cst);}
 ~PageWrite39(){page_epoch39.fetch_add(1,std::memory_order_seq_cst);page_mutex42.unlock();}
};
}

#include <chrono>
#include <cstdio>
namespace {
template<int Tag> struct WorkProbe37 {
 explicit WorkProbe37(const char*) {}
 void request(unsigned, unsigned, unsigned) {}
 void lock_begin() {}
 void locked() {}
 void scanned() {}
 void uploading() {}
};
}
#include <span>
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::graphics {
int request_source78 = 0;
unsigned frame_seq98 = 1;  // 1 while the command processor requests vertex buffers
namespace {
std::atomic<unsigned long long> cpu_invalidations78{0}, cpu_invalidated_pages78{0};
struct Churn78 {
  unsigned long long calls = 0, pages = 0, repeat = 0, vertex_pages = 0, vertex_repeat = 0, uploads = 0;
  uint16_t last_gen[32768] = {};  // per 16 KB page: generation of its last upload (0 = never)
  std::chrono::steady_clock::time_point report = std::chrono::steady_clock::now();
};
Churn78& churn78() { static Churn78 c; return c; }
struct Hot80 {
  uint8_t state[32768] = {};  // 0-2 repeat score, 3 hot, 0xFF disqualified
  uint8_t* shadow[32768] = {};
  unsigned stamp[32768] = {};  // frame_seq98 of the last compare
  uint32_t count = 0;
  bool in_split = false;
  unsigned long long compares = 0, equal = 0, changed = 0, promotions = 0, demotions = 0, bytes116 = 0;
  std::vector<std::pair<uint32_t, uint32_t>> scratch;
};
Hot80& hot80() { static Hot80 h; return h; }
bool Disqualify80(uint32_t page) {  // returns whether the page was hot
  Hot80& h = hot80();
  if (page >= 32768 || h.state[page] == 0xFF) return false;
  const bool was_hot = h.state[page] == 3;
  if (was_hot) {
    delete[] h.shadow[page];
    h.shadow[page] = nullptr;
    --h.count;
    ++h.demotions;
  }
  h.state[page] = 0xFF;
  return was_hot;
}
unsigned hot_valid_seq99 = 0;
std::vector<uint32_t> hot_touched99, hot_cleared99;
}  // namespace

// Build 61: guards watch lists (global_watches_, watch_buckets_, pools) and,
// through the callbacks, texture outdated flags. Recursive: texture code holds
// it around WatchMemoryRange.
std::unique_lock<std::recursive_mutex> AcquireSharedMemoryWatchLock61() {
  static std::recursive_mutex mutex;
  return std::unique_lock<std::recursive_mutex>(mutex);
}

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  PageWrite39 write39;
  for (size_t i=0;i<system_page_flags_valid_.size();++i) {
    __atomic_store_n(&system_page_flags_valid_[i],uint64_t(0),__ATOMIC_RELEASE);
    __atomic_store_n(&system_page_flags_valid_and_gpu_written_[i],uint64_t(0),__ATOMIC_RELEASE);
  }
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  PageWrite39 write39;
  for(size_t i=0;i<system_page_flags_valid_.size();++i)
    __atomic_store_n(&system_page_flags_valid_[i],__atomic_load_n(&system_page_flags_valid_and_gpu_written_[i],__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = AcquireSharedMemoryWatchLock61();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = AcquireSharedMemoryWatchLock61();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = AcquireSharedMemoryWatchLock61();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = AcquireSharedMemoryWatchLock61();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = AcquireSharedMemoryWatchLock61();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  if (hot80().count && !hot80().in_split) {
    Hot80& h = hot80();
    const uint32_t len80 = std::min(length, kBufferSize - start);
    const uint32_t pf80 = start >> page_size_log2_;
    const uint32_t pl80 = (start + len80 - 1) >> page_size_log2_;
    if (written_by_gpu) {
      for (uint32_t p = pf80; p <= pl80; ++p) Disqualify80(p);
    } else {
      bool any_hot80 = false;
      for (uint32_t p = pf80; p <= pl80 && !any_hot80; ++p) any_hot80 = p < 32768 && h.state[p] == 3;
      if (any_hot80) {
        // Validate and arm only the runs of non-hot pages.
        h.in_split = true;
        uint32_t run80 = UINT32_MAX;
        for (uint32_t p = pf80; p <= pl80 + 1; ++p) {
          const bool hot = p <= pl80 && p < 32768 && h.state[p] == 3;
          const bool end = p > pl80 || hot;
          if (!end && run80 == UINT32_MAX) run80 = p;
          if (end && run80 != UINT32_MAX) {
            MakeRangeValid(run80 << page_size_log2_, (p - run80) << page_size_log2_, false);
            run80 = UINT32_MAX;
          }
        }
        h.in_split = false;
        return;
      }
    }
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    // PageWrite39 provides the page-flag lock; no watch callbacks occur here.

    PageWrite39 write39;
    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      __atomic_fetch_or(&system_page_flags_valid_[i],valid_bits,__ATOMIC_RELEASE);
      if(written_by_gpu)__atomic_fetch_or(&system_page_flags_valid_and_gpu_written_[i],valid_bits,__ATOMIC_RELEASE);
      else __atomic_fetch_and(&system_page_flags_valid_and_gpu_written_[i],~valid_bits,__ATOMIC_RELEASE);
    }
  }

  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}


namespace {
// Build 146: byte-equality of two ranges (no ordering), 32 bytes at a time.
inline bool Equal146(const uint8_t* a, const uint8_t* b, size_t n) {
  size_t i = 0;
  for (; i + 128 <= n; i += 128) {
    __m256i x0 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i)),
                                  _mm256_loadu_si256((const __m256i*)(b + i)));
    __m256i x1 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i + 32)),
                                  _mm256_loadu_si256((const __m256i*)(b + i + 32)));
    __m256i x2 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i + 64)),
                                  _mm256_loadu_si256((const __m256i*)(b + i + 64)));
    __m256i x3 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i + 96)),
                                  _mm256_loadu_si256((const __m256i*)(b + i + 96)));
    __m256i x = _mm256_or_si256(_mm256_or_si256(x0, x1), _mm256_or_si256(x2, x3));
    if (!_mm256_testz_si256(x, x)) return false;
  }
  for (; i + 32 <= n; i += 32) {
    __m256i x = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i)),
                                 _mm256_loadu_si256((const __m256i*)(b + i)));
    if (!_mm256_testz_si256(x, x)) return false;
  }
  for (; i < n; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}
}  // namespace
bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
 WorkProbe37<1> work37("shared_range");
 {
   Churn78& ch = churn78();
   if ((++ch.calls & 4095) == 0) {
     const auto now = std::chrono::steady_clock::now();
     if (now - ch.report >= std::chrono::seconds(10)) {
       std::fprintf(stderr,
                    "[FABLE2-CHURN78] uploads=%llu pages=%llu repeat_2f=%llu vertex_pages=%llu "
                    "vertex_repeat_2f=%llu cpu_invalidations=%llu cpu_pages=%llu\n",
                    ch.uploads, ch.pages, ch.repeat, ch.vertex_pages, ch.vertex_repeat,
                    cpu_invalidations78.exchange(0, std::memory_order_relaxed),
                    cpu_invalidated_pages78.exchange(0, std::memory_order_relaxed));
       std::fflush(stderr);
       Hot80& h = hot80();
       std::fprintf(stderr,
                    "[FABLE2-HOT80] hot=%u promotions=%llu demotions=%llu compares=%llu equal=%llu "
                    "changed=%llu bytes116=%llu\n",
                    h.count, h.promotions, h.demotions, h.compares, h.equal, h.changed, h.bytes116);
       std::fflush(stderr);
       h.promotions = h.demotions = h.compares = h.equal = h.changed = h.bytes116 = 0;
       ch.uploads = ch.pages = ch.repeat = ch.vertex_pages = ch.vertex_repeat = 0;
       ch.report = now;
     }
   }
 }
 if(ranges && count==1)work37.request(ranges[0].first,ranges[0].second,kBufferSize);
  if (ranges == nullptr || !count) {
    return true;
  }

  // A single range needs no sorting, merging, or temporary allocation.
  std::vector<std::pair<uint32_t, uint32_t>> multiple_ranges;
  std::span<const std::pair<uint32_t, uint32_t>> merged_ranges;
  if (count == 1) {
    const uint32_t start = ranges[0].first;
    const uint32_t length = ranges[0].second;
    if (!length) return true;
    if (start > kBufferSize || (kBufferSize - start) < length) return false;
    merged_ranges = std::span<const std::pair<uint32_t, uint32_t>>(ranges, 1);
  } else {
  // Some texture or buffer is empty, for example - safe to draw in this case.
  multiple_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    multiple_ranges.emplace_back(start, length);
  }
  if (multiple_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(multiple_ranges.begin(), multiple_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < multiple_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = multiple_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = multiple_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      multiple_ranges[++merged_write] = range_current;
    }
  }
  multiple_ranges.resize(merged_write + 1);

    merged_ranges = multiple_ranges;
  }

  if (request_source78 != 1 && request_source78 != 2) {
    static unsigned long long disq97[4] = {}, calls97 = 0;
    ++disq97[request_source78 & 3];
    if ((++calls97 & ((1u << 18) - 1)) == 0) {
      std::fprintf(stderr, "[FABLE2-DISQ97] other=%llu texture=%llu hot=%u\n", disq97[0], disq97[3],
                   hot80().count);
      std::fflush(stderr);
      disq97[0] = disq97[3] = 0;
    }
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      const uint32_t pf80 = range.first >> page_size_log2_;
      const uint32_t pl80 = (range.first + range.second - 1) >> page_size_log2_;
      for (uint32_t p = pf80; p <= pl80; ++p) {
        if (Disqualify80(p)) hot_cleared99.push_back(p);
      }
    }
    if (!hot_cleared99.empty()) {
      PageWrite39 write99;
      for (uint32_t p : hot_cleared99) {
        __atomic_fetch_and(&system_page_flags_valid_[p >> 6], ~(uint64_t(1) << (p & 63)), __ATOMIC_RELEASE);
      }
      hot_cleared99.clear();
    }
  }
  // Build 99: new frame -> hot pages must be compared again.
  if (hot_valid_seq99 != frame_seq98) {
    hot_valid_seq99 = frame_seq98;
    if (hot80().count) {
      Hot80& h99 = hot80();
      PageWrite39 write99;
      for (uint32_t p = 0; p < 32768; ++p) {
        if (h99.state[p] == 3) {
          __atomic_fetch_and(&system_page_flags_valid_[p >> 6], ~(uint64_t(1) << (p & 63)), __ATOMIC_RELEASE);
        }
      }
    }
  }
  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  // Return only if every requested page was valid in one stable epoch.
  const auto before39=page_epoch39.load(std::memory_order_acquire);
  bool valid39=(before39&1u)==0;
  if(valid39) for(const auto& range:merged_ranges) {
    const uint32_t first=range.first>>page_size_log2_;
    const uint32_t last=(range.first+range.second-1)>>page_size_log2_;
    for(uint32_t block=first>>6;block<=last>>6;++block) {
      uint64_t mask=UINT64_MAX;
      if(block==(first>>6))mask&=UINT64_MAX<<(first&63);
      if(block==(last>>6)&&(last&63)!=63)mask&=(uint64_t(1)<<((last&63)+1))-1;
      if((__atomic_load_n(&system_page_flags_valid_[block],__ATOMIC_ACQUIRE)&mask)!=mask){valid39=false;break;}
    }
    if(!valid39)break;
  }
  valid39=valid39 && page_epoch39.load(std::memory_order_acquire)==before39;
  static thread_local unsigned long long hits39=0,misses39=0;
  if(valid39)++hits39;else ++misses39;
  if(((hits39+misses39)&262143u)==0){std::fprintf(stderr,"[FABLE2-VALID-FAST] hit=%llu fallback=%llu\n",hits39,misses39);std::fflush(stderr);}
  if(valid39)return true;

  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    work37.lock_begin();
    std::unique_lock<std::mutex> global_lock(page_mutex42);
    work37.locked();
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = __atomic_load_n(&system_page_flags_valid_[i],__ATOMIC_ACQUIRE);
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  work37.scanned();
  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (!upload_ranges_.empty() && hot80().count) {
    Hot80& h = hot80();
    const uint32_t psize80 = uint32_t(1) << page_size_log2_;
    std::vector<std::pair<uint32_t, uint32_t>>& kept80 = h.scratch;
    kept80.clear();
    for (const std::pair<uint32_t, uint32_t>& up : upload_ranges_) {
      for (uint32_t p = up.first; p < up.first + up.second; ++p) {
        if (p < 32768 && h.state[p] == 3) {
          // Build 116: compare exactly the requested bytes of this page, on
          // every request; hot pages are never marked valid.
          const uint64_t pb116 = uint64_t(p) << page_size_log2_;
          const uint64_t pe116 = pb116 + psize80;
          const uint8_t* guest80 =
              reinterpret_cast<const uint8_t*>(memory().TranslatePhysical(uint32_t(pb116)));
          bool same116 = true;
          for (const std::pair<uint32_t, uint32_t>& r116 : merged_ranges) {
            const uint64_t a116 = std::max<uint64_t>(r116.first, pb116);
            const uint64_t b116 = std::min<uint64_t>(uint64_t(r116.first) + r116.second, pe116);
            if (a116 >= b116) continue;
            ++h.compares;
            h.bytes116 += b116 - a116;
            if (!Equal146(guest80 + (a116 - pb116), h.shadow[p] + (a116 - pb116),
                            size_t(b116 - a116))) {
              same116 = false;
              break;
            }
          }
          if (same116) {
            ++h.equal;
            continue;
          }
          std::memcpy(h.shadow[p], guest80, psize80);
          ++h.changed;
        }
        if (!kept80.empty() && kept80.back().first + kept80.back().second == p) {
          ++kept80.back().second;
        } else {
          kept80.emplace_back(p, 1);
        }
      }
    }
    upload_ranges_.swap(kept80);
    if (!hot_touched99.empty()) {
      PageWrite39 write99;
      for (uint32_t p : hot_touched99) {
        __atomic_fetch_or(&system_page_flags_valid_[p >> 6], uint64_t(1) << (p & 63), __ATOMIC_RELEASE);
      }
      hot_touched99.clear();
    }
  }

  if (upload_ranges_.empty()) {
    return true;
  }

  {
    Churn78& ch = churn78();
    // A generation is 16384 RequestRanges calls, about two frames in Bowerstone.
    const uint16_t gen = uint16_t((ch.calls >> 14) + 1);
    ++ch.uploads;
    for (const std::pair<uint32_t, uint32_t>& up : upload_ranges_) {
      for (uint32_t p = up.first; p < up.first + up.second && p < 32768; ++p) {
        const bool again = ch.last_gen[p] && uint16_t(gen - ch.last_gen[p]) <= 1;
        ++ch.pages;
        ch.repeat += again;
        if (request_source78 == 1) {
          ++ch.vertex_pages;
          ch.vertex_repeat += again;
        }
        ch.last_gen[p] = gen;
        if ((request_source78 == 1 || request_source78 == 2) && again && p < 32768) {
          Hot80& h = hot80();
          uint8_t st80 = h.state[p];
          if (st80 < 2) {
            h.state[p] = st80 + 1;
          } else if (st80 == 2 && h.count < 2048) {
            const uint32_t psize80 = uint32_t(1) << page_size_log2_;
            h.shadow[p] = new uint8_t[psize80];
            std::memcpy(h.shadow[p],
                        reinterpret_cast<const uint8_t*>(memory().TranslatePhysical(p << page_size_log2_)),
                        psize80);
            h.state[p] = 3;
            ++h.count;
            ++h.promotions;
          }
        }
      }
    }
  }
  work37.uploading();
  // Build 52: no global critical region here. Write-watch arming inside
  // MakeRangeValid is serialized by the memory system's dedicated watch lock,
  // which is all UploadRanges needs (as in upstream Xenia before batch 49).
  return UploadRanges(upload_ranges_);
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {
    // Bound excess invalidation to four host pages (64KB on PS5), not 64 pages.
    // Stop at GPU-written pages so their newer contents cannot be lost.
    const uint32_t lower51=page_first & ~uint32_t(3);
    while(page_first>lower51) {
      const uint32_t p51=page_first-1;
      const uint64_t gpu51=__atomic_load_n(&system_page_flags_valid_and_gpu_written_[p51>>6],__ATOMIC_ACQUIRE);
      if(gpu51 & (uint64_t(1)<<(p51&63)))break;
      --page_first;
    }
    const uint32_t upper51=page_last | uint32_t(3);
    while(page_last<upper51) {
      const uint32_t p51=page_last+1;
      const uint64_t gpu51=__atomic_load_n(&system_page_flags_valid_and_gpu_written_[p51>>6],__ATOMIC_ACQUIRE);
      if(gpu51 & (uint64_t(1)<<(p51&63)))break;
      ++page_last;
    }
  }

  { PageWrite39 write39;
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    __atomic_fetch_and(&system_page_flags_valid_[i],~invalidate_bits,__ATOMIC_RELEASE);
    __atomic_fetch_and(&system_page_flags_valid_and_gpu_written_[i],~invalidate_bits,__ATOMIC_RELEASE);
  }
  }

  cpu_invalidations78.fetch_add(1, std::memory_order_relaxed);
  cpu_invalidated_pages78.fetch_add(page_last - page_first + 1, std::memory_order_relaxed);
  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
