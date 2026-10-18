// xecache_merge: validate and merge ReXGlue Vulkan shader (.xsh) and pipeline (.xpso)
// storage files without ever writing a partial or corrupt record.
//
//   xecache_merge info FILE...          print header and valid/invalid record counts
//   xecache_merge merge OUT IN1 [IN2..] write OUT = union of valid records (by hash),
//                                       in input order; all inputs must share a header
//
// Formats (ReXGlueSDK include/rex/graphics/vulkan/pipeline_cache.h,
// src/graphics/vulkan/pipeline_cache.cpp):
//   .xsh : u32 magic 'XESH' (0x48534558), u32 version (byte-swapped)
//          then records { u64 ucode_data_hash; u32 dword_count:31, type:1; u32 ucode[] }
//          with ucode_data_hash = XXH3_64bits(ucode)
//   .xpso: u32 magic 'XEPS' (0x53504558), u32 api (0 FBO / 1 FSI), u32 version (byte-swapped)
//          then 66-byte records { u64 description_hash; u8 description[58] }
//          with description_hash = XXH3_64bits(description)
// The loaders stop at the first record that fails its hash, so OUT only contains
// records whose hash verifies.
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XSH_MAGIC 0x48534558u
#define XPSO_MAGIC 0x53504558u
#define XPSO_RECORD 66u

typedef struct {
  uint8_t* data;
  size_t size;
} Blob;

static Blob read_file(const char* path) {
  Blob b = {0, 0};
  FILE* f = fopen(path, "rb");
  if (!f) return b;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    b.data = malloc((size_t)n);
    b.size = fread(b.data, 1, (size_t)n, f);
  }
  fclose(f);
  return b;
}

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

// Simple open-addressing set of 64-bit keys.
typedef struct { uint64_t* k; uint8_t* used; size_t cap; } Set;
static void set_init(Set* s, size_t n) {
  s->cap = 16; while (s->cap < n * 2 + 16) s->cap <<= 1;
  s->k = calloc(s->cap, 8); s->used = calloc(s->cap, 1);
}
static int set_add(Set* s, uint64_t key) {  // 1 if newly added
  size_t i = (size_t)(key * 0x9E3779B97F4A7C15ull) & (s->cap - 1);
  while (s->used[i]) { if (s->k[i] == key) return 0; i = (i + 1) & (s->cap - 1); }
  s->used[i] = 1; s->k[i] = key; return 1;
}

static int header_size(const Blob* b) {
  if (b->size < 8) return -1;
  uint32_t magic = rd32(b->data);
  if (magic == XSH_MAGIC) return 8;
  if (magic == XPSO_MAGIC && b->size >= 12) return 12;
  return -1;
}

// Calls fn(record_ptr, record_len, key) for each valid record; returns count, sets *bad.
typedef void (*RecordFn)(const uint8_t*, size_t, uint64_t, void*);
static size_t walk(const Blob* b, RecordFn fn, void* ctx, size_t* bad) {
  int h = header_size(b);
  size_t n = 0, off = (size_t)h;
  *bad = 0;
  if (h < 0) return 0;
  if (rd32(b->data) == XPSO_MAGIC) {
    while (off + XPSO_RECORD <= b->size) {
      const uint8_t* r = b->data + off;
      if (XXH3_64bits(r + 8, XPSO_RECORD - 8) != rd64(r)) { *bad = (b->size - off) / XPSO_RECORD; break; }
      if (fn) fn(r, XPSO_RECORD, rd64(r), ctx);
      ++n; off += XPSO_RECORD;
    }
  } else {
    while (off + 12 <= b->size) {
      const uint8_t* r = b->data + off;
      uint32_t word = rd32(r + 8), count = word & 0x7FFFFFFFu;
      size_t len = 12 + (size_t)count * 4;
      if (off + len > b->size) { *bad = 1; break; }
      if (XXH3_64bits(r + 12, (size_t)count * 4) != rd64(r)) { *bad = 1; break; }
      // Key: hash mixed with the shader type bit.
      if (fn) fn(r, len, rd64(r) ^ ((uint64_t)(word >> 31) << 63), ctx);
      ++n; off += len;
    }
  }
  return n;
}

typedef struct { FILE* out; Set* seen; size_t written; } MergeCtx;
static void emit(const uint8_t* r, size_t len, uint64_t key, void* p) {
  MergeCtx* m = p;
  if (set_add(m->seen, key)) { fwrite(r, 1, len, m->out); ++m->written; }
}

int main(int argc, char** argv) {
  if (argc >= 3 && !strcmp(argv[1], "info")) {
    for (int i = 2; i < argc; ++i) {
      Blob b = read_file(argv[i]);
      int h = header_size(&b);
      if (h < 0) { printf("%s: not a storage file (%zu bytes)\n", argv[i], b.size); continue; }
      size_t bad = 0, n = walk(&b, NULL, NULL, &bad);
      printf("%s: %s header=%08x/%08x%s records=%zu trailing_invalid=%zu bytes=%zu\n", argv[i],
             rd32(b.data) == XPSO_MAGIC ? "xpso" : "xsh", rd32(b.data + 4),
             h == 12 ? rd32(b.data + 8) : 0, h == 12 ? "" : " ", n, bad, b.size);
      free(b.data);
    }
    return 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "merge")) {
    Blob first = read_file(argv[3]);
    int h = header_size(&first);
    if (h < 0) { fprintf(stderr, "%s: not a storage file\n", argv[3]); return 1; }
    FILE* out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fwrite(first.data, 1, (size_t)h, out);
    Set seen; set_init(&seen, first.size / 12 + 4096);
    MergeCtx m = {out, &seen, 0};
    for (int i = 3; i < argc; ++i) {
      Blob b = i == 3 ? first : read_file(argv[i]);
      if (header_size(&b) != h || memcmp(b.data, first.data, (size_t)h)) {
        fprintf(stderr, "%s: header differs from %s - skipped\n", argv[i], argv[3]);
        continue;
      }
      size_t bad = 0, before = m.written, n = walk(&b, emit, &m, &bad);
      printf("%s: %zu valid (%zu new), %zu trailing invalid\n", argv[i], n, m.written - before, bad);
    }
    fclose(out);
    printf("%s: %zu records\n", argv[2], m.written);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "index")) {
    // Hash-only shader index for shader recovery on the console (no shader code):
    //   u32 'F2SI', u32 version 1, u32 count, then per shader
    //   { u64 ucode_hash; u64 prefix_hash (XXH3 of the first 32 bytes); u32 dword_count; u32 type }
    Blob b = read_file(argv[2]);
    if (header_size(&b) != 8) { fprintf(stderr, "%s: not a .xsh file\n", argv[2]); return 1; }
    FILE* out = fopen(argv[3], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
    uint32_t head[3] = {0x49533246u, 1u, 0u};
    fwrite(head, 4, 3, out);
    size_t off = 8, bad = 0;
    uint32_t count = 0, skipped_short = 0;
    while (off + 12 <= b.size) {
      const uint8_t* r = b.data + off;
      uint32_t word = rd32(r + 8), dwords = word & 0x7FFFFFFFu;
      size_t len = 12 + (size_t)dwords * 4;
      if (off + len > b.size || XXH3_64bits(r + 12, (size_t)dwords * 4) != rd64(r)) { bad = 1; break; }
      if (dwords * 4 >= 32) {
        uint64_t hash = rd64(r), prefix = XXH3_64bits(r + 12, 32);
        uint32_t type = word >> 31;
        fwrite(&hash, 8, 1, out); fwrite(&prefix, 8, 1, out);
        fwrite(&dwords, 4, 1, out); fwrite(&type, 4, 1, out);
        ++count;
      } else {
        ++skipped_short;
      }
      off += len;
    }
    fseek(out, 8, SEEK_SET);
    fwrite(&count, 4, 1, out);
    fclose(out);
    printf("%s: %u shaders indexed, %u too short (<32 bytes), trailing_invalid=%zu\n", argv[3], count,
           skipped_short, bad);
    return 0;
  }
  fprintf(stderr, "usage: xecache_merge info FILE... | merge OUT IN1 [IN2...] | index XSH OUT\n");
  return 2;
}
