// Roundtrip test: PDFium render → PNG encode → PNG decode → pixel comparison.
// Verifies fastpdf2png's PNG encoding pipeline is lossless.
// Usage: test_roundtrip <pdf_file> [dpi]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#include "fpdfview.h"
#include "png_writer.h"

// stb_image for PNG decoding (header-only)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

static constexpr float kPointsPerInch = 72.0f;

struct PageResult {
  bool identical;
  int diff_pixels;
  int max_diff;
};

PageResult TestPage(FPDF_DOCUMENT doc, int page_idx, float dpi, int compression) {
  auto* page = FPDF_LoadPage(doc, page_idx);
  if (!page) return {false, -1, -1};

  const auto scale = dpi / kPointsPerInch;
  const auto width  = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
  const auto height = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
  const auto stride = (width * 4 + 63) & ~63;
  const auto buf_size = static_cast<size_t>(stride) * height;

  std::vector<uint8_t> buffer(buf_size);
  auto* bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx,
                                      buffer.data(), stride);
  if (!bitmap) { FPDF_ClosePage(page); return {false, -1, -1}; }

  FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0,
                        FPDF_ANNOT | FPDF_PRINTING | FPDF_NO_CATCH);

  // Encode to PNG in memory
  uint8_t* png_data = nullptr;
  size_t png_size = 0;
  auto rc = fast_png::WriteBgraToMemory(buffer.data(), width, height, stride,
                                         &png_data, &png_size, compression);
  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);

  if (rc != fast_png::kSuccess || !png_data) return {false, -1, -1};

  // Decode PNG back
  int dec_w, dec_h, dec_ch;
  auto* decoded = stbi_load_from_memory(png_data, static_cast<int>(png_size),
                                         &dec_w, &dec_h, &dec_ch, 3);
  std::free(png_data);

  if (!decoded) return {false, -1, -1};

  // Compare pixels: original BGRx → RGB vs decoded RGB
  int diff_pixels = 0;
  int max_diff = 0;

  for (int y = 0; y < height; ++y) {
    const auto* src = buffer.data() + y * stride;
    const auto* dec = decoded + y * width * 3;

    for (int x = 0; x < width; ++x) {
      // BGRx → RGB
      const int orig_r = src[x * 4 + 2];
      const int orig_g = src[x * 4 + 1];
      const int orig_b = src[x * 4 + 0];

      const int dec_r = dec[x * 3 + 0];
      const int dec_g = dec[x * 3 + 1];
      const int dec_b = dec[x * 3 + 2];

      // For grayscale pages, PNG stores as 8-bit gray.
      // Decoded as RGB: R=G=B=gray. Original BGRx: B=G=R when gray.
      // So comparison should still hold.

      const int dr = std::abs(orig_r - dec_r);
      const int dg = std::abs(orig_g - dec_g);
      const int db = std::abs(orig_b - dec_b);
      const int d = std::max({dr, dg, db});

      if (d > 0) {
        ++diff_pixels;
        if (d > max_diff) max_diff = d;
      }
    }
  }

  stbi_image_free(decoded);
  return {diff_pixels == 0, diff_pixels, max_diff};
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <pdf> [dpi] [compression]\n", argv[0]);
    return 1;
  }

  const char* pdf_path = argv[1];
  const float dpi = argc > 2 ? std::atof(argv[2]) : 150.0f;
  const int compression = argc > 3 ? std::atoi(argv[3]) : 2;

  FPDF_InitLibrary();

  auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) {
    std::fprintf(stderr, "Failed to open: %s\n", pdf_path);
    FPDF_DestroyLibrary();
    return 1;
  }

  const int pages = FPDF_GetPageCount(doc);
  int total_identical = 0;
  int total_failures = 0;

  for (int i = 0; i < pages; ++i) {
    auto result = TestPage(doc, i, dpi, compression);
    if (result.identical) {
      ++total_identical;
    } else {
      ++total_failures;
      std::printf("  FAIL page %d: %d pixels differ, max_diff=%d\n",
                  i + 1, result.diff_pixels, result.max_diff);
    }
  }

  FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();

  std::printf("%s: %d/%d pages pixel-identical (compression=%d, dpi=%.0f)\n",
              pdf_path, total_identical, pages, compression, dpi);

  return total_failures > 0 ? 1 : 0;
}
