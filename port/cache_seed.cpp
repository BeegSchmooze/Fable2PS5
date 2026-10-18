// Fable 2 (PS5): shader cache seeding and shader recovery.
//
// The repository never ships the game's shader code. It ships pipeline descriptions (render
// state plus shader hashes, `cache-seed/4D5307F1.*.vk.xpso`) and a hash-only shader index
// (`cache-seed/4D5307F1.shader-index.bin`). On the console:
//
//  - SeedOnBoot(), before the emulator opens its cache, merges the shipped pipeline
//    descriptions into the console's pipeline file and appends any shaders recovered by an
//    earlier run to the console's shader file. The emulator then translates every stored
//    shader and builds every stored pipeline whose shaders it has at startup, so they never
//    hitch in play. A pipeline whose shader is still missing is skipped harmlessly.
//  - StartShaderRecovery() runs a background thread that scans committed guest physical
//    memory for shaders listed in the index (a hash of the first 32 bytes finds candidates,
//    the full hash proves them) and saves those found to `4D5307F1.recovered.xsh` for the
//    next boot. The shaders come from the player's own copy of the game.
//
// File formats (ReXGlueSDK vulkan/pipeline_cache.cpp):
//   .xsh : u32 'XESH', u32 version (byte-swapped), records
//          { u64 XXH3(ucode); u32 dword_count:31, type:1; u32 ucode[dword_count] }
//   .xpso: u32 'XEPS', u32 api, u32 version (byte-swapped), 66-byte records
//          { u64 XXH3(description); u8 description[58] }
//   index: u32 'F2SI', u32 version 1, u32 count, records
//          { u64 ucode_hash; u64 prefix_hash; u32 dword_count; u32 type }
// The ucode hash is taken over the guest's big-endian words exactly as they sit in memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

namespace fable2_cache {
namespace {

constexpr const char* kCacheDir = "/app0/cache/shaders/shareable/";
constexpr const char* kSeedDir = "/app0/cache-seed/";
constexpr const char* kTitle = "4D5307F1";
constexpr uint32_t kXshMagic = 0x48534558u;
constexpr uint32_t kXpsoMagic = 0x53504558u;
constexpr uint32_t kIndexMagic = 0x49533246u;
constexpr size_t kXpsoRecord = 66;
constexpr size_t kPrefixBytes = 32;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[FABLE2-SHADERS] %s\n", line);
  std::fflush(stderr);
}

std::string CachePath(const char* suffix) { return std::string(kCacheDir) + kTitle + suffix; }
std::string SeedPath(const char* suffix) { return std::string(kSeedDir) + kTitle + suffix; }

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  out.clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t buffer[65536];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.insert(out.end(), buffer, buffer + n);
  std::fclose(f);
  return true;
}

// Writes to a temporary name and renames it over the target, so a crash never leaves a
// half-written cache file behind.
bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& data) {
  std::string temporary = path + ".tmp";
  FILE* f = std::fopen(temporary.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = (std::fflush(f) == 0) && ok;
  ok = (std::fclose(f) == 0) && ok;
  if (!ok || std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return false;
  }
  return true;
}

uint32_t Read32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t Read64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

struct XshRecord {
  uint64_t hash;
  size_t offset;  // of the 12-byte record header
  size_t length;  // header + ucode
};

// Returns the valid records of a .xsh image (stops at the first bad one, as the emulator does).
bool ParseXsh(const std::vector<uint8_t>& file, std::vector<XshRecord>& records) {
  records.clear();
  if (file.size() < 8 || Read32(file.data()) != kXshMagic) return false;
  size_t offset = 8;
  while (offset + 12 <= file.size()) {
    const uint8_t* r = file.data() + offset;
    uint32_t dwords = Read32(r + 8) & 0x7FFFFFFFu;
    size_t length = 12 + size_t(dwords) * 4;
    if (offset + length > file.size() || XXH3_64bits(r + 12, size_t(dwords) * 4) != Read64(r)) break;
    records.push_back({Read64(r), offset, length});
    offset += length;
  }
  return true;
}

// Valid 66-byte records of a .xpso image.
size_t ValidXpsoRecords(const std::vector<uint8_t>& file) {
  if (file.size() < 12 || Read32(file.data()) != kXpsoMagic) return 0;
  size_t count = 0;
  for (size_t offset = 12; offset + kXpsoRecord <= file.size(); offset += kXpsoRecord) {
    const uint8_t* r = file.data() + offset;
    if (XXH3_64bits(r + 8, kXpsoRecord - 8) != Read64(r)) break;
    ++count;
  }
  return count;
}

void MergePipelines(const char* suffix) {
  std::vector<uint8_t> seed, cache;
  if (!ReadFile(SeedPath(suffix), seed)) return;
  size_t seed_count = ValidXpsoRecords(seed);
  if (!seed_count) {
    Log("seed %s%s is not valid, ignored", kTitle, suffix);
    return;
  }
  bool have_cache = ReadFile(CachePath(suffix), cache);
  size_t cache_count = have_cache ? ValidXpsoRecords(cache) : 0;
  if (!have_cache || cache.size() < 12 || std::memcmp(cache.data(), seed.data(), 12) != 0) {
    // No cache yet, or one from another emulator version (the emulator would discard it).
    seed.resize(12 + seed_count * kXpsoRecord);
    if (WriteFileAtomic(CachePath(suffix), seed)) Log("pipelines %s: installed %zu from seed", suffix, seed_count);
    return;
  }
  std::vector<uint8_t> merged(cache.begin(), cache.begin() + 12 + cache_count * kXpsoRecord);
  std::unordered_set<uint64_t> known;
  for (size_t i = 0; i < cache_count; ++i) known.insert(Read64(cache.data() + 12 + i * kXpsoRecord));
  size_t added = 0;
  for (size_t i = 0; i < seed_count; ++i) {
    const uint8_t* r = seed.data() + 12 + i * kXpsoRecord;
    if (known.insert(Read64(r)).second) {
      merged.insert(merged.end(), r, r + kXpsoRecord);
      ++added;
    }
  }
  if (added && WriteFileAtomic(CachePath(suffix), merged)) {
    Log("pipelines %s: console had %zu, added %zu from seed", suffix, cache_count, added);
  }
}

void MergeRecoveredShaders() {
  std::vector<uint8_t> recovered, cache;
  if (!ReadFile(CachePath(".recovered.xsh"), recovered)) return;
  std::vector<XshRecord> recovered_records, cache_records;
  if (!ParseXsh(recovered, recovered_records) || recovered_records.empty()) {
    std::remove(CachePath(".recovered.xsh").c_str());
    return;
  }
  bool have_cache = ReadFile(CachePath(".xsh"), cache) && ParseXsh(cache, cache_records) &&
                    std::memcmp(cache.data(), recovered.data(), 8) == 0;
  std::vector<uint8_t> merged;
  std::unordered_set<uint64_t> known;
  if (have_cache) {
    size_t valid_end = cache_records.empty() ? 8 : cache_records.back().offset + cache_records.back().length;
    merged.assign(cache.begin(), cache.begin() + valid_end);
    for (const XshRecord& r : cache_records) known.insert(r.hash);
  } else {
    merged.assign(recovered.begin(), recovered.begin() + 8);
  }
  size_t added = 0;
  for (const XshRecord& r : recovered_records) {
    if (known.insert(r.hash).second) {
      merged.insert(merged.end(), recovered.begin() + r.offset, recovered.begin() + r.offset + r.length);
      ++added;
    }
  }
  if (!added || WriteFileAtomic(CachePath(".xsh"), merged)) {
    std::remove(CachePath(".recovered.xsh").c_str());
    Log("shaders: console had %zu, added %zu recovered", cache_records.size(), added);
  }
}

struct IndexEntry {
  uint64_t hash;
  uint64_t prefix;
  uint32_t dwords;
  uint32_t type;
};

bool LoadIndex(std::vector<IndexEntry>& entries) {
  std::vector<uint8_t> file;
  if (!ReadFile(SeedPath(".shader-index.bin"), file) || file.size() < 12 ||
      Read32(file.data()) != kIndexMagic || Read32(file.data() + 4) != 1) {
    return false;
  }
  uint32_t count = Read32(file.data() + 8);
  if (file.size() < 12 + size_t(count) * 24) return false;
  entries.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = file.data() + 12 + size_t(i) * 24;
    entries[i] = {Read64(r), Read64(r + 8), Read32(r + 16), Read32(r + 20)};
  }
  return true;
}

class Recovery {
 public:
  bool Prepare() {
    if (!LoadIndex(index_)) return false;
    for (size_t i = 0; i < index_.size(); ++i) by_prefix_.emplace(index_[i].prefix, i);
    filter_.assign(kFilterBits / 64, 0);
    for (const IndexEntry& e : index_) {
      filter_[(e.prefix & (kFilterBits - 1)) / 64] |= uint64_t(1) << (e.prefix & 63);
      max_bytes_ = std::max(max_bytes_, size_t(e.dwords) * 4);
    }
    // Shaders saved by an earlier scan this session or already in the console's cache.
    std::vector<uint8_t> file;
    std::vector<XshRecord> records;
    if (ReadFile(CachePath(".xsh"), file) && ParseXsh(file, records)) {
      std::memcpy(header_, file.data(), 8);
      have_header_ = true;
      for (const XshRecord& r : records) known_.insert(r.hash);
    }
    return true;
  }

  size_t missing() const {
    size_t n = 0;
    for (const IndexEntry& e : index_) n += !known_.count(e.hash);
    return n;
  }

  // One pass over committed physical memory. Returns the number of new shaders found.
  size_t Scan() {
    rex::memory::Memory* memory = rex::system::kernel_memory();
    if (!memory) return 0;
    rex::memory::VirtualHeap* heap = memory->GetPhysicalHeap();
    const uint64_t begin = heap->heap_base(), end = begin + uint64_t(heap->heap_size());
    std::vector<uint8_t> chunk;
    size_t found_before = found_.size();
    uint64_t scanned = 0;
    for (uint64_t address = begin; address < end;) {
      rex::memory::HeapAllocationInfo info{};
      {
        auto lock = rex::thread::global_critical_region::AcquireDirect();
        if (!heap->QueryRegionInfo(uint32_t(address), &info)) break;
      }
      uint64_t region_end = uint64_t(info.base_address) + info.region_size;
      if (info.region_size == 0 || region_end <= address) {
        address += 4096;
        continue;
      }
      bool readable = (info.state & rex::memory::kMemoryAllocationCommit) &&
                      (info.protect & rex::memory::kMemoryProtectRead);
      if (readable) {
        for (uint64_t at = address; at < region_end; at += kChunk) {
          size_t body = size_t(std::min<uint64_t>(kChunk, region_end - at));
          size_t with_tail = size_t(std::min<uint64_t>(kChunk + max_bytes_, region_end - at));
          if (!CopyChunk(memory, heap, uint32_t(at), with_tail, chunk)) break;
          ScanChunk(chunk, body);
          scanned += body;
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      }
      address = region_end;
    }
    size_t found_now = found_.size() - found_before;
    Log("scan: %.0f MB checked, %zu new shaders, %zu of %zu still missing", scanned / 1048576.0,
        found_now, missing(), index_.size());
    if (found_now) SaveRecovered();
    return found_now;
  }

 private:
  static constexpr uint64_t kFilterBits = uint64_t(1) << 22;
  static constexpr uint64_t kChunk = 1u << 20;

  // Copies guest physical memory while holding the global lock, so the pages cannot be
  // decommitted underneath the copy.
  bool CopyChunk(rex::memory::Memory* memory, rex::memory::VirtualHeap* heap, uint32_t address,
                 size_t bytes, std::vector<uint8_t>& out) {
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(address, &info) || !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead) ||
        uint64_t(info.base_address) + info.region_size < uint64_t(address) + bytes) {
      return false;
    }
    out.resize(bytes);
    std::memcpy(out.data(), memory->TranslatePhysical(address), bytes);
    return true;
  }

  void ScanChunk(const std::vector<uint8_t>& chunk, size_t body) {
    if (chunk.size() < kPrefixBytes) return;
    size_t last = std::min(body, chunk.size() - kPrefixBytes + 1);
    for (size_t offset = 0; offset < last; offset += 4) {
      uint64_t prefix = XXH3_64bits(chunk.data() + offset, kPrefixBytes);
      if (!(filter_[(prefix & (kFilterBits - 1)) / 64] & (uint64_t(1) << (prefix & 63)))) continue;
      auto range = by_prefix_.equal_range(prefix);
      for (auto it = range.first; it != range.second; ++it) {
        const IndexEntry& e = index_[it->second];
        size_t bytes = size_t(e.dwords) * 4;
        if (known_.count(e.hash) || offset + bytes > chunk.size()) continue;
        if (XXH3_64bits(chunk.data() + offset, bytes) != e.hash) continue;
        known_.insert(e.hash);
        std::vector<uint8_t> record(12 + bytes);
        uint32_t word = e.dwords | (e.type << 31);
        std::memcpy(record.data(), &e.hash, 8);
        std::memcpy(record.data() + 8, &word, 4);
        std::memcpy(record.data() + 12, chunk.data() + offset, bytes);
        found_.push_back(std::move(record));
      }
    }
  }

  void SaveRecovered() {
    std::vector<uint8_t> file(8);
    if (have_header_) {
      std::memcpy(file.data(), header_, 8);
    } else {
      // 'XESH' and the emulator's shader storage version 0x20201219, byte-swapped.
      const uint32_t header[2] = {kXshMagic, 0x19122020u};
      std::memcpy(file.data(), header, 8);
    }
    for (const std::vector<uint8_t>& record : found_) file.insert(file.end(), record.begin(), record.end());
    if (WriteFileAtomic(CachePath(".recovered.xsh"), file)) {
      Log("saved %zu recovered shaders for the next start", found_.size());
    }
  }

  std::vector<IndexEntry> index_;
  std::unordered_multimap<uint64_t, size_t> by_prefix_;
  std::vector<uint64_t> filter_;
  std::unordered_set<uint64_t> known_;
  std::vector<std::vector<uint8_t>> found_;
  size_t max_bytes_ = 0;
  uint8_t header_[8] = {};
  bool have_header_ = false;
};

}  // namespace

void SeedOnBoot() {
  MergePipelines(".fbo.vk.xpso");
  MergePipelines(".fsi.vk.xpso");
  MergeRecoveredShaders();
}

void StartShaderRecovery() {
  std::thread([] {
    Recovery recovery;
    if (!recovery.Prepare()) {
      Log("no shader index in %s, recovery off", kSeedDir);
      return;
    }
    if (!recovery.missing()) {
      Log("all %s indexed shaders already cached", kTitle);
      return;
    }
    // The game loads its shaders as it starts and as areas stream in; look a few times.
    const int delays_s[] = {60, 120, 300, 600, 1200, 2400};
    for (int delay : delays_s) {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      if (!rex::system::kernel_state()) continue;
      recovery.Scan();
      if (!recovery.missing()) break;
    }
  }).detach();
}

}  // namespace fable2_cache
