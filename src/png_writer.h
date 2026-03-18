// fastpdf2png - Ultra-fast PDF to PNG converter
// SPDX-License-Identifier: MIT

#ifndef FASTPDF2PNG_FAST_PNG_WRITER_H_
#define FASTPDF2PNG_FAST_PNG_WRITER_H_

#include <stdint.h>
#include <stddef.h>

// Error codes
#define FAST_PNG_SUCCESS 0
#define FAST_PNG_ERROR_INVALID_PARAMS 1
#define FAST_PNG_ERROR_ALLOC_FAILED 2
#define FAST_PNG_ERROR_COMPRESS_FAILED 3
#define FAST_PNG_ERROR_FILE_OPEN_FAILED 4
#define FAST_PNG_ERROR_FILE_WRITE_FAILED 5

// Compression levels
#define FAST_PNG_COMPRESS_FAST   0  // fpng (largest files, fastest)
#define FAST_PNG_COMPRESS_MEDIUM 1  // fpng slower (~6% smaller)
#define FAST_PNG_COMPRESS_BEST   2  // libdeflate (smallest files)

// Write BGRA pixel buffer to PNG file.
// Auto-detects grayscale pages and outputs 8-bit grayscale (1/3 data).
// Uses thread-local buffers — zero allocation after warmup.
int FastPngWriteBgra(const char* filename,
                     const uint8_t* pixels,
                     int width, int height, int stride,
                     int compression_level = FAST_PNG_COMPRESS_FAST);

// Write BGRA pixel buffer to PNG in memory.
int FastPngWriteBgraToMemory(const uint8_t* pixels,
                             int width, int height, int stride,
                             uint8_t** out_data, size_t* out_size,
                             int compression_level = FAST_PNG_COMPRESS_FAST);

#endif  // FASTPDF2PNG_FAST_PNG_WRITER_H_
