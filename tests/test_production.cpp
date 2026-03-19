// Production hardening tests for fastpdf2png.
// Tests edge cases: corrupted PDFs, empty files, huge pages, bad args, etc.
// Usage: test_production

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <atomic>

#include "fpdfview.h"
#include "png_writer.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool ok, const char* name) {
  if (ok) {
    std::printf("  PASS  %s\n", name);
    ++g_pass;
  } else {
    std::printf("  FAIL  %s\n", name);
    ++g_fail;
  }
}

// Helper: run fastpdf2png as subprocess, return exit code
static int RunBinary(const std::string& binary, const std::vector<std::string>& args,
                     std::string* out = nullptr) {
  std::string cmd = binary;
  for (const auto& a : args) cmd += " \"" + a + "\"";
  cmd += " 2>&1";
  auto* fp = popen(cmd.c_str(), "r");
  if (!fp) return -1;
  char buf[4096];
  std::string output;
  while (fgets(buf, sizeof(buf), fp)) output += buf;
  if (out) *out = output;
  return pclose(fp) >> 8;
}

// Helper: create temp file with given content
static std::string WriteTempFile(const std::string& name, const void* data, size_t size) {
  auto path = fs::temp_directory_path() / name;
  std::ofstream f(path, std::ios::binary);
  f.write(static_cast<const char*>(data), size);
  return path.string();
}

// ============================================================
// Test categories
// ============================================================

static void TestCorruptedPdfs(const std::string& binary) {
  std::printf("\n--- Corrupted/malformed PDFs ---\n");
  auto tmpdir = fs::temp_directory_path() / "fastpdf2png_test";
  fs::create_directories(tmpdir);
  auto pattern = (tmpdir / "page_%03d.png").string();

  // Empty file
  {
    auto path = WriteTempFile("empty.pdf", "", 0);
    auto rc = RunBinary(binary, {path, pattern, "150", "1"});
    Check(rc != 0, "Empty file returns error");
    fs::remove(path);
  }

  // Random garbage
  {
    std::vector<uint8_t> garbage(1024);
    for (auto& b : garbage) b = rand() % 256;
    auto path = WriteTempFile("garbage.pdf", garbage.data(), garbage.size());
    auto rc = RunBinary(binary, {path, pattern, "150", "1"});
    Check(rc != 0, "Random garbage returns error");
    fs::remove(path);
  }

  // Valid PDF header but truncated
  {
    const char* header = "%PDF-1.4\n";
    auto path = WriteTempFile("truncated.pdf", header, strlen(header));
    auto rc = RunBinary(binary, {path, pattern, "150", "1"});
    Check(rc != 0, "Truncated PDF returns error");
    fs::remove(path);
  }

  // Huge garbage file (1MB)
  {
    std::vector<uint8_t> big(1024 * 1024);
    memcpy(big.data(), "%PDF-1.7\n", 9);
    for (size_t i = 9; i < big.size(); i++) big[i] = rand() % 256;
    auto path = WriteTempFile("big_garbage.pdf", big.data(), big.size());
    auto rc = RunBinary(binary, {path, pattern, "150", "1"});
    Check(rc != 0, "Large garbage PDF returns error");
    fs::remove(path);
  }

  // Non-existent file
  {
    auto rc = RunBinary(binary, {"/nonexistent/path/fake.pdf", pattern, "150", "1"});
    Check(rc != 0, "Non-existent file returns error");
  }

  fs::remove_all(tmpdir);
}

static void TestBadArguments(const std::string& binary) {
  std::printf("\n--- Bad arguments ---\n");
  auto tmpdir = fs::temp_directory_path() / "fastpdf2png_test";
  fs::create_directories(tmpdir);

  // No arguments
  {
    auto rc = RunBinary(binary, {});
    Check(rc != 0, "No arguments returns error/usage");
  }

  // Only input, no output pattern
  {
    auto rc = RunBinary(binary, {"some.pdf"});
    Check(rc != 0, "Missing output pattern returns error");
  }

  // DPI = 0
  {
    auto path = WriteTempFile("dummy.pdf", "%PDF-1.4", 8);
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {path, pattern, "0", "1"});
    Check(rc != 0, "DPI=0 returns error");
    fs::remove(path);
  }

  // Negative workers
  {
    auto path = WriteTempFile("dummy.pdf", "%PDF-1.4", 8);
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {path, pattern, "150", "-1"});
    Check(rc != 0, "Workers=-1 returns error");
    fs::remove(path);
  }

  // Invalid compression
  {
    auto path = WriteTempFile("dummy.pdf", "%PDF-1.4", 8);
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {path, pattern, "150", "1", "-c", "99"});
    Check(rc != 0, "Invalid compression level returns error");
    fs::remove(path);
  }

  fs::remove_all(tmpdir);
}

static void TestInfoMode(const std::string& binary, const std::string& pdf) {
  std::printf("\n--- Info mode ---\n");

  // --info on valid PDF
  {
    std::string out;
    auto rc = RunBinary(binary, {"--info", pdf}, &out);
    Check(rc == 0, "--info on valid PDF succeeds");
    int pages = std::atoi(out.c_str());
    Check(pages > 0, "--info returns positive page count");
  }

  // --info on non-existent
  {
    auto rc = RunBinary(binary, {"--info", "/nonexistent.pdf"});
    Check(rc != 0, "--info on missing file returns error");
  }

  // --info on garbage
  {
    auto path = WriteTempFile("garbage.pdf", "not a pdf", 9);
    auto rc = RunBinary(binary, {"--info", path});
    Check(rc != 0, "--info on garbage returns error");
    fs::remove(path);
  }
}

static void TestOutputValidity(const std::string& binary, const std::string& pdf) {
  std::printf("\n--- Output PNG validity ---\n");
  auto tmpdir = fs::temp_directory_path() / "fastpdf2png_test_out";
  fs::create_directories(tmpdir);
  auto pattern = (tmpdir / "page_%03d.png").string();

  // Render
  auto rc = RunBinary(binary, {pdf, pattern, "150", "4"});
  Check(rc == 0, "Render succeeds");

  // Verify each PNG is valid
  int valid = 0, invalid = 0;
  for (const auto& entry : fs::directory_iterator(tmpdir)) {
    if (entry.path().extension() != ".png") continue;
    int w, h, ch;
    auto* data = stbi_load(entry.path().c_str(), &w, &h, &ch, 3);
    if (data && w > 0 && h > 0) {
      ++valid;
      stbi_image_free(data);
    } else {
      ++invalid;
      std::printf("    Invalid PNG: %s\n", entry.path().filename().c_str());
    }
  }
  Check(valid > 0, "At least one valid PNG produced");
  Check(invalid == 0, "All PNGs are valid/decodable");

  fs::remove_all(tmpdir);
}

static void TestHighDpi(const std::string& binary, const std::string& pdf) {
  std::printf("\n--- High DPI / large output ---\n");
  auto tmpdir = fs::temp_directory_path() / "fastpdf2png_test_dpi";
  fs::create_directories(tmpdir);

  // DPI 600 — produces ~5000x6500 pixels per page
  {
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {pdf, pattern, "600", "1"});
    Check(rc == 0, "DPI 600 render succeeds");

    // Check first page dimensions
    auto first = tmpdir / "page_001.png";
    if (fs::exists(first)) {
      int w, h, ch;
      auto* data = stbi_load(first.c_str(), &w, &h, &ch, 3);
      Check(data != nullptr && w > 4000 && h > 5000, "DPI 600 produces large image");
      if (data) stbi_image_free(data);
    }
  }

  fs::remove_all(tmpdir);
}

static void TestConcurrentInvocations(const std::string& binary, const std::string& pdf) {
  std::printf("\n--- Concurrent invocations ---\n");
  constexpr int kConcurrent = 4;
  std::atomic<int> ok_count{0};
  std::atomic<int> fail_count{0};

  auto run = [&](int id) {
    auto tmpdir = fs::temp_directory_path() / ("fastpdf2png_conc_" + std::to_string(id));
    fs::create_directories(tmpdir);
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {pdf, pattern, "150", "4"});
    if (rc == 0) ++ok_count; else ++fail_count;
    fs::remove_all(tmpdir);
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kConcurrent; ++i)
    threads.emplace_back(run, i);
  for (auto& t : threads) t.join();

  Check(ok_count == kConcurrent, "All concurrent invocations succeed");
  Check(fail_count == 0, "No concurrent failures");
}

static void TestWorkerCounts(const std::string& binary, const std::string& pdf) {
  std::printf("\n--- Worker count variations ---\n");
  auto tmpdir = fs::temp_directory_path() / "fastpdf2png_test_workers";

  for (int w : {1, 2, 4, 8, 16, 32}) {
    fs::create_directories(tmpdir);
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {pdf, pattern, "150", std::to_string(w)});
    auto label = std::to_string(w) + " workers render succeeds";
    Check(rc == 0, label.c_str());

    // Count output files
    int count = 0;
    for (const auto& e : fs::directory_iterator(tmpdir))
      if (e.path().extension() == ".png") ++count;

    auto count_label = std::to_string(w) + " workers produce correct page count";
    // Get expected page count
    std::string info_out;
    RunBinary(binary, {"--info", pdf}, &info_out);
    int expected = std::atoi(info_out.c_str());
    Check(count == expected, count_label.c_str());

    fs::remove_all(tmpdir);
  }
}

static void TestCompressionLevels(const std::string& binary, const std::string& pdf) {
  std::printf("\n--- Compression levels ---\n");
  auto tmpdir = fs::temp_directory_path() / "fastpdf2png_test_comp";

  for (int c : {0, 1, 2}) {
    fs::create_directories(tmpdir);
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {pdf, pattern, "150", "1", "-c", std::to_string(c)});
    auto label = "Compression " + std::to_string(c) + " succeeds";
    Check(rc == 0, label.c_str());

    // Verify first PNG valid
    auto first = tmpdir / "page_001.png";
    int w, h, ch;
    auto* data = stbi_load(first.c_str(), &w, &h, &ch, 3);
    auto vlabel = "Compression " + std::to_string(c) + " output valid";
    Check(data != nullptr, vlabel.c_str());
    if (data) stbi_image_free(data);

    fs::remove_all(tmpdir);
  }
}

static void TestOutputPath(const std::string& binary, const std::string& pdf) {
  std::printf("\n--- Output path edge cases ---\n");

  // Non-existent output directory
  {
    auto rc = RunBinary(binary, {pdf, "/nonexistent/dir/page_%03d.png", "150", "1"});
    Check(rc != 0, "Non-existent output dir returns error");
  }

  // Read-only directory
  {
    auto tmpdir = fs::temp_directory_path() / "fastpdf2png_readonly";
    fs::create_directories(tmpdir);
    fs::permissions(tmpdir, fs::perms::owner_read | fs::perms::owner_exec);
    auto pattern = (tmpdir / "page_%03d.png").string();
    auto rc = RunBinary(binary, {pdf, pattern, "150", "1"});
    Check(rc != 0, "Read-only output dir returns error");
    fs::permissions(tmpdir, fs::perms::all);
    fs::remove_all(tmpdir);
  }
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::fprintf(stderr, "Usage: %s <fastpdf2png_binary> <test_pdf>\n", argv[0]);
    return 1;
  }

  const std::string binary = argv[1];
  const std::string pdf = argv[2];

  std::printf("=== Production Hardening Tests ===\n");
  std::printf("Binary: %s\n", binary.c_str());
  std::printf("PDF:    %s\n", pdf.c_str());

  FPDF_InitLibrary();

  TestCorruptedPdfs(binary);
  TestBadArguments(binary);
  TestInfoMode(binary, pdf);
  TestOutputValidity(binary, pdf);
  TestHighDpi(binary, pdf);
  TestConcurrentInvocations(binary, pdf);
  TestWorkerCounts(binary, pdf);
  TestCompressionLevels(binary, pdf);
  TestOutputPath(binary, pdf);

  FPDF_DestroyLibrary();

  std::printf("\n==========================================\n");
  std::printf("Results: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
