// fastpdf2png - Ultra-fast PNG encoder
// SPDX-License-Identifier: MIT
//
// Key optimizations:
// - NEON (ARM) / SSE+AVX2 (x86) SIMD for pixel conversion
// - Auto grayscale detection (1/3 data for B/W pages)
// - Thread-local libdeflate compressor (zero alloc per page)
// - Zero-copy: compress directly into output buffer
// - Single write() syscall with F_NOCACHE on macOS
// - libdeflate CRC32 (SIMD-accelerated)

#include "png_writer.h"
#include "memory_pool.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <mutex>

#include "libdeflate.h"
#include "fpng/fpng.h"

// ARM NEON
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define USE_NEON 1
#endif

// x86 SIMD
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#if defined(__AVX2__)
#define USE_AVX2 1
#endif
#define USE_SSE 1
#endif

namespace {

void EnsureFpngInit() {
  static std::once_flag flag;
  std::call_once(flag, fpng::fpng_init);
}

// Thread-local libdeflate compressor (reused across pages)
struct libdeflate_compressor* GetThreadLocalCompressor() {
  static thread_local struct libdeflate_compressor* comp = nullptr;
  if (!comp)
    comp = libdeflate_alloc_compressor(1);
  return comp;
}

// Thread-local reusable buffers (zero alloc after warmup)
struct ThreadLocalBuffers {
  uint8_t* raw = nullptr;
  uint8_t* comp = nullptr;
  size_t raw_cap = 0;
  size_t comp_cap = 0;

  uint8_t* AcquireRaw(size_t size) {
    if (size > raw_cap) {
      free(raw);
      raw_cap = size + size / 4;
      raw = static_cast<uint8_t*>(malloc(raw_cap));
    }
    return raw;
  }

  uint8_t* AcquireComp(size_t size) {
    if (size > comp_cap) {
      free(comp);
      comp_cap = size + size / 4;
      comp = static_cast<uint8_t*>(malloc(comp_cap));
    }
    return comp;
  }
};

ThreadLocalBuffers& GetThreadLocalBuffers() {
  static thread_local ThreadLocalBuffers bufs;
  return bufs;
}

// ---------- BGRA→RGBA conversion (only used for fpng path) ----------

#if USE_AVX2
static const __m256i avx2_shuffle = _mm256_setr_epi8(
    2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
    2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);

void BgraToRgba(const uint8_t* src, uint8_t* dst, int width, int height,
                int src_stride) {
  const int dst_stride = width * 4;
  for (int y = 0; y < height; y++) {
    const uint8_t* s = src + y * src_stride;
    uint8_t* d = dst + y * dst_stride;
    int x = 0;
    for (; x + 8 <= width; x += 8) {
      __m256i p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + x*4));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + x*4),
                          _mm256_shuffle_epi8(p, avx2_shuffle));
    }
    const __m128i sse_shuf = _mm_setr_epi8(2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
    for (; x + 4 <= width; x += 4) {
      __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x*4));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x*4),
                       _mm_shuffle_epi8(p, sse_shuf));
    }
    for (; x < width; x++) {
      d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3];
    }
  }
}
#elif USE_SSE
static const __m128i shuffle_mask = _mm_setr_epi8(
    2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);

void BgraToRgba(const uint8_t* src, uint8_t* dst, int width, int height,
                int src_stride) {
  const int dst_stride = width * 4;
  for (int y = 0; y < height; y++) {
    const uint8_t* s = src + y * src_stride;
    uint8_t* d = dst + y * dst_stride;
    int x = 0;
    for (; x + 4 <= width; x += 4) {
      __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x*4));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x*4),
                       _mm_shuffle_epi8(p, shuffle_mask));
    }
    for (; x < width; x++) {
      d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3];
    }
  }
}
#else
void BgraToRgba(const uint8_t* src, uint8_t* dst, int width, int height,
                int src_stride) {
  const int dst_stride = width * 4;
  for (int y = 0; y < height; y++) {
    const uint8_t* s = src + y * src_stride;
    uint8_t* d = dst + y * dst_stride;
    for (int x = 0; x < width; x++) {
      d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3];
    }
  }
}
#endif

// ---------- BGRA→RGB row conversion ----------

inline void ConvertRowBgraToRgb(uint8_t* dst, const uint8_t* src, int width) {
  int x = 0;
#if USE_NEON
  for (; x + 16 <= width; x += 16) {
    uint8x16x4_t bgra = vld4q_u8(src + x * 4);
    uint8x16x3_t rgb;
    rgb.val[0] = bgra.val[2];
    rgb.val[1] = bgra.val[1];
    rgb.val[2] = bgra.val[0];
    vst3q_u8(dst + x * 3, rgb);
  }
#elif USE_SSE
  const __m128i shuf = _mm_setr_epi8(2,1,0, 6,5,4, 10,9,8, 14,13,12, -1,-1,-1,-1);
  for (; x + 4 <= width; x += 4) {
    __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
    __m128i rgb = _mm_shuffle_epi8(p, shuf);
    // Store only 12 valid bytes (not 16) to avoid overwriting next row's filter byte
    uint8_t tmp[16];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(tmp), rgb);
    memcpy(dst + x * 3, tmp, 12);
  }
#endif
  for (; x < width; x++) {
    dst[x*3] = src[x*4+2]; dst[x*3+1] = src[x*4+1]; dst[x*3+2] = src[x*4];
  }
}

// ---------- Grayscale detection (NEON-accelerated) ----------

inline bool IsRowGrayscale(const uint8_t* src, int width) {
  int x = 0;
#if USE_NEON
  for (; x + 16 <= width; x += 16) {
    uint8x16x4_t bgra = vld4q_u8(src + x * 4);
    uint8x16_t eq_bg = vceqq_u8(bgra.val[0], bgra.val[1]);
    uint8x16_t eq_gr = vceqq_u8(bgra.val[1], bgra.val[2]);
    if (vminvq_u8(vandq_u8(eq_bg, eq_gr)) != 0xFF) return false;
  }
#endif
  for (; x < width; x++) {
    if (src[x*4] != src[x*4+1] || src[x*4+1] != src[x*4+2])
      return false;
  }
  return true;
}

bool IsImageGrayscale(const uint8_t* pixels, int width, int height, int stride) {
  // Quick reject: check first, middle, last rows
  if (!IsRowGrayscale(pixels, width)) return false;
  if (!IsRowGrayscale(pixels + (height/2) * stride, width)) return false;
  if (!IsRowGrayscale(pixels + (height-1) * stride, width)) return false;
  // Full scan
  for (int y = 1; y < height - 1; y++) {
    if (!IsRowGrayscale(pixels + y * stride, width))
      return false;
  }
  return true;
}

// ---------- Pixel → raw PNG data conversion ----------

void ConvertToGrayscaleRawPng(uint8_t* raw, const uint8_t* pixels,
                               int width, int height, int stride) {
  size_t row_bytes = 1 + static_cast<size_t>(width);
  for (int y = 0; y < height; y++) {
    raw[y * row_bytes] = 0;  // Filter: None
    uint8_t* dst = raw + y * row_bytes + 1;
    const uint8_t* src = pixels + y * stride;
    int x = 0;
#if USE_NEON
    for (; x + 16 <= width; x += 16) {
      uint8x16x4_t bgra = vld4q_u8(src + x * 4);
      vst1q_u8(dst + x, bgra.val[0]);
    }
#endif
    for (; x < width; x++)
      dst[x] = src[x * 4];
  }
}

void ConvertToRgbRawPng(uint8_t* raw, const uint8_t* pixels,
                         int width, int height, int stride) {
  size_t row_bytes = 1 + static_cast<size_t>(width) * 3;
  for (int y = 0; y < height; y++) {
    raw[y * row_bytes] = 0;  // Filter: None
    ConvertRowBgraToRgb(raw + y * row_bytes + 1, pixels + y * stride, width);
  }
}

// ---------- PNG assembly ----------

const uint8_t kPngSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
constexpr size_t kPngHeaderSize = 41;  // sig(8) + IHDR(25) + IDAT len+type(8)
constexpr size_t kPngTrailerSize = 16; // IDAT CRC(4) + IEND(12)

void WriteU32Be(uint8_t* p, uint32_t v) {
  p[0] = (v >> 24); p[1] = (v >> 16); p[2] = (v >> 8); p[3] = v;
}

size_t FinalizePng(uint8_t* buf, int width, int height,
                   size_t compressed_size, int color_type) {
  uint8_t* p = buf;

  memcpy(p, kPngSignature, 8);
  p += 8;

  // IHDR
  WriteU32Be(p, 13);
  memcpy(p + 4, "IHDR", 4);
  WriteU32Be(p + 8, width);
  WriteU32Be(p + 12, height);
  p[16] = 8; p[17] = color_type; p[18] = 0; p[19] = 0; p[20] = 0;
  WriteU32Be(p + 21, libdeflate_crc32(0, p + 4, 17));
  p += 25;

  // IDAT (data already in place at p + 8)
  WriteU32Be(p, static_cast<uint32_t>(compressed_size));
  memcpy(p + 4, "IDAT", 4);
  WriteU32Be(p + 8 + compressed_size,
             libdeflate_crc32(0, p + 4, 4 + compressed_size));
  p += 8 + compressed_size + 4;

  // IEND
  static const uint8_t kIend[12] = {0,0,0,0, 'I','E','N','D', 0xAE,0x42,0x60,0x82};
  memcpy(p, kIend, 12);
  p += 12;

  return p - buf;
}

// ---------- Core write paths ----------

int WritePngLibdeflate(const char* filename, const uint8_t* pixels,
                       int width, int height, int stride) {
  bool grayscale = IsImageGrayscale(pixels, width, height, stride);

  int color_type;
  size_t row_bytes;
  if (grayscale) {
    row_bytes = 1 + static_cast<size_t>(width);
    color_type = 0;
  } else {
    row_bytes = 1 + static_cast<size_t>(width) * 3;
    color_type = 2;
  }

  size_t raw_size = row_bytes * height;
  ThreadLocalBuffers& bufs = GetThreadLocalBuffers();
  uint8_t* raw_data = bufs.AcquireRaw(raw_size);
  if (!raw_data) return FAST_PNG_ERROR_ALLOC_FAILED;

  if (grayscale)
    ConvertToGrayscaleRawPng(raw_data, pixels, width, height, stride);
  else
    ConvertToRgbRawPng(raw_data, pixels, width, height, stride);

  struct libdeflate_compressor* c = GetThreadLocalCompressor();
  if (!c) return FAST_PNG_ERROR_ALLOC_FAILED;
  size_t bound = libdeflate_zlib_compress_bound(c, raw_size);

  size_t out_size = kPngHeaderSize + bound + kPngTrailerSize;
  uint8_t* out = bufs.AcquireComp(out_size);
  if (!out) return FAST_PNG_ERROR_ALLOC_FAILED;

  size_t comp_size = libdeflate_zlib_compress(c, raw_data, raw_size,
                                               out + kPngHeaderSize, bound);
  if (comp_size == 0) return FAST_PNG_ERROR_COMPRESS_FAILED;

  size_t png_size = FinalizePng(out, width, height, comp_size, color_type);

  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return FAST_PNG_ERROR_FILE_OPEN_FAILED;
#ifdef __APPLE__
  fcntl(fd, F_NOCACHE, 1);
#endif
  ssize_t written = write(fd, out, png_size);
  close(fd);

  return (written == static_cast<ssize_t>(png_size))
             ? FAST_PNG_SUCCESS : FAST_PNG_ERROR_FILE_WRITE_FAILED;
}

int WritePngLibdeflateToMemory(const uint8_t* pixels, int width, int height,
                                int stride, uint8_t** out_data, size_t* out_size) {
  bool grayscale = IsImageGrayscale(pixels, width, height, stride);

  int color_type;
  size_t row_bytes;
  if (grayscale) {
    row_bytes = 1 + static_cast<size_t>(width);
    color_type = 0;
  } else {
    row_bytes = 1 + static_cast<size_t>(width) * 3;
    color_type = 2;
  }

  size_t raw_size = row_bytes * height;
  ThreadLocalBuffers& bufs = GetThreadLocalBuffers();
  uint8_t* raw_data = bufs.AcquireRaw(raw_size);
  if (!raw_data) return FAST_PNG_ERROR_ALLOC_FAILED;

  if (grayscale)
    ConvertToGrayscaleRawPng(raw_data, pixels, width, height, stride);
  else
    ConvertToRgbRawPng(raw_data, pixels, width, height, stride);

  struct libdeflate_compressor* c = GetThreadLocalCompressor();
  if (!c) return FAST_PNG_ERROR_ALLOC_FAILED;
  size_t bound = libdeflate_zlib_compress_bound(c, raw_size);

  size_t total = kPngHeaderSize + bound + kPngTrailerSize;
  *out_data = static_cast<uint8_t*>(malloc(total));
  if (!*out_data) return FAST_PNG_ERROR_ALLOC_FAILED;

  size_t comp_size = libdeflate_zlib_compress(c, raw_data, raw_size,
                                               *out_data + kPngHeaderSize, bound);
  if (comp_size == 0) { free(*out_data); *out_data = nullptr; return FAST_PNG_ERROR_COMPRESS_FAILED; }

  *out_size = FinalizePng(*out_data, width, height, comp_size, color_type);
  return FAST_PNG_SUCCESS;
}

}  // namespace

// ---------- Public API ----------

int FastPngWriteBgra(const char* filename, const uint8_t* pixels,
                     int width, int height, int stride,
                     int compression_level) {
  if (!filename || !pixels || width <= 0 || height <= 0)
    return FAST_PNG_ERROR_INVALID_PARAMS;

  if (compression_level == FAST_PNG_COMPRESS_BEST)
    return WritePngLibdeflate(filename, pixels, width, height, stride);

  // fpng path: needs RGBA
  EnsureFpngInit();
  const size_t rgba_size = static_cast<size_t>(width) * height * 4;
  fast_png::PageMemoryPool& pool = fast_png::GetThreadLocalPool();
  uint8_t* rgba = pool.Acquire(rgba_size);
  if (!rgba) return FAST_PNG_ERROR_ALLOC_FAILED;

  BgraToRgba(pixels, rgba, width, height, stride);

  int flags = (compression_level >= FAST_PNG_COMPRESS_MEDIUM)
              ? fpng::FPNG_ENCODE_SLOWER : 0;
  return fpng::fpng_encode_image_to_file(filename, rgba, width, height, 4, flags)
             ? FAST_PNG_SUCCESS : FAST_PNG_ERROR_COMPRESS_FAILED;
}

int FastPngWriteBgraToMemory(const uint8_t* pixels, int width, int height,
                              int stride, uint8_t** out_data, size_t* out_size,
                              int compression_level) {
  if (!pixels || width <= 0 || height <= 0 || !out_data || !out_size)
    return FAST_PNG_ERROR_INVALID_PARAMS;

  if (compression_level == FAST_PNG_COMPRESS_BEST)
    return WritePngLibdeflateToMemory(pixels, width, height, stride, out_data, out_size);

  EnsureFpngInit();
  const size_t rgba_size = static_cast<size_t>(width) * height * 4;
  fast_png::PageMemoryPool& pool = fast_png::GetThreadLocalPool();
  uint8_t* rgba = pool.Acquire(rgba_size);
  if (!rgba) return FAST_PNG_ERROR_ALLOC_FAILED;

  BgraToRgba(pixels, rgba, width, height, stride);

  int flags = (compression_level >= FAST_PNG_COMPRESS_MEDIUM)
              ? fpng::FPNG_ENCODE_SLOWER : 0;
  std::vector<uint8_t> buf;
  if (!fpng::fpng_encode_image_to_memory(rgba, width, height, 4, buf, flags))
    return FAST_PNG_ERROR_COMPRESS_FAILED;

  *out_size = buf.size();
  *out_data = static_cast<uint8_t*>(malloc(*out_size));
  if (!*out_data) return FAST_PNG_ERROR_ALLOC_FAILED;
  memcpy(*out_data, buf.data(), *out_size);
  return FAST_PNG_SUCCESS;
}
