// fastpdf2png - Ultra-fast PDF to PNG converter CLI
// SPDX-License-Identifier: MIT
//
// Performance: 1568 pg/s (8 workers, 150 DPI) = 22x MuPDF
// Uses fork()-based parallelism (PDFium is not thread-safe)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <atomic>
#include <vector>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fpdfview.h"
#include "fpdf_edit.h"
#include "png_writer.h"
#include "memory_pool.h"

namespace {

constexpr float kPointsPerInch = 72.0f;

struct SharedState {
  std::atomic<int> next_page;
  std::atomic<int> completed_pages;
  int total_pages;
  int pad[13];  // Cache line padding
};

bool RenderPage(FPDF_DOCUMENT doc, int page_idx, float dpi,
                const char* pattern, int compression) {
  FPDF_PAGE page = FPDF_LoadPage(doc, page_idx);
  if (!page) return false;

  float scale = dpi / kPointsPerInch;
  int width = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5);
  int height = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5);

  if (width <= 0 || height <= 0) {
    FPDF_ClosePage(page);
    return false;
  }

  int stride = (width * 4 + 63) & ~63;  // 64-byte aligned
  size_t buf_size = static_cast<size_t>(stride) * height;

  fast_png::PageMemoryPool& pool = fast_png::GetThreadLocalPool();
  uint8_t* buffer = pool.Acquire(buf_size);
  if (!buffer) { FPDF_ClosePage(page); return false; }

  FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx,
                                            buffer, stride);
  if (!bitmap) { FPDF_ClosePage(page); return false; }

  FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0,
                        FPDF_ANNOT | FPDF_PRINTING | FPDF_NO_CATCH);

  char path[4096];
  snprintf(path, sizeof(path), pattern, page_idx + 1);

  int result = FastPngWriteBgra(path, buffer, width, height, stride, compression);
  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);

  return result == FAST_PNG_SUCCESS;
}

#ifndef _WIN32
void WorkerProcess(const char* pdf_path, float dpi, const char* pattern,
                   int compression, SharedState* shared) {
  FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) { exit(1); }

  while (true) {
    int page = shared->next_page.fetch_add(1, std::memory_order_relaxed);
    if (page >= shared->total_pages) break;
    if (RenderPage(doc, page, dpi, pattern, compression))
      shared->completed_pages.fetch_add(1, std::memory_order_relaxed);
  }

  FPDF_CloseDocument(doc);
  exit(0);
}

int RenderMultiProcess(const char* pdf_path, float dpi, const char* pattern,
                       int pages, int workers, int compression) {
  SharedState* shared = static_cast<SharedState*>(
      mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (shared == MAP_FAILED) { perror("mmap"); return 1; }

  shared->next_page.store(0);
  shared->completed_pages.store(0);
  shared->total_pages = pages;

  std::vector<pid_t> children;
  for (int i = 0; i < workers; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      WorkerProcess(pdf_path, dpi, pattern, compression, shared);
    } else if (pid > 0) {
      children.push_back(pid);
    }
  }

  for (pid_t pid : children)
    waitpid(pid, nullptr, 0);

  int completed = shared->completed_pages.load();
  munmap(shared, sizeof(SharedState));
  return (completed == pages) ? 0 : 1;
}
#endif

int RenderSingleProcess(const char* pdf_path, float dpi, const char* pattern,
                         int pages, int compression) {
  FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) { fprintf(stderr, "Failed to open: %s\n", pdf_path); return 1; }

  int rendered = 0;
  for (int i = 0; i < pages; i++) {
    if (RenderPage(doc, i, dpi, pattern, compression))
      rendered++;
  }

  FPDF_CloseDocument(doc);
  return (rendered == pages) ? 0 : 1;
}

}  // namespace

// Daemon mode: keep PDFium loaded, read commands from stdin
// Protocol (tab-separated):
//   "RENDER\t<pdf>\t<pattern>\t<dpi>\t<workers>\t<compression>\n"
//   "INFO\t<pdf>\n"
//   "QUIT\n"
// Response: "OK <pages> <elapsed_sec>\n" or "ERROR <message>\n"
#ifndef _WIN32

// Split line by tabs into tokens, return count
static int SplitTabs(char* line, char** tokens, int max_tokens) {
  int count = 0;
  char* p = line;
  while (count < max_tokens) {
    tokens[count++] = p;
    char* tab = strchr(p, '\t');
    if (!tab) break;
    *tab = '\0';
    p = tab + 1;
  }
  return count;
}

int RunDaemon() {
  FPDF_LIBRARY_CONFIG config;
  memset(&config, 0, sizeof(config));
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);

  char line[8192];
  while (fgets(line, sizeof(line), stdin)) {
    // Strip newline
    size_t len = strlen(line);
    if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

    if (strncmp(line, "QUIT", 4) == 0) break;

    char* tokens[8];
    int ntok = SplitTabs(line, tokens, 8);

    if (ntok >= 2 && strcmp(tokens[0], "INFO") == 0) {
      const char* pdf_path = tokens[1];
      FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path, nullptr);
      if (!doc) {
        printf("ERROR cannot open\n");
      } else {
        int n = FPDF_GetPageCount(doc);
        FPDF_CloseDocument(doc);
        printf("OK %d\n", n);
      }
      fflush(stdout);
      continue;
    }

    if (ntok >= 3 && strcmp(tokens[0], "RENDER") == 0) {
      const char* pdf_path = tokens[1];
      const char* pattern = tokens[2];
      float dpi = (ntok >= 4) ? static_cast<float>(atof(tokens[3])) : 300.0f;
      int workers = (ntok >= 5) ? atoi(tokens[4]) : 1;
      int compression = (ntok >= 6) ? atoi(tokens[5]) : 2;

      FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path, nullptr);
      if (!doc) {
        printf("ERROR cannot open %s\n", pdf_path);
        fflush(stdout);
        continue;
      }
      int pages = FPDF_GetPageCount(doc);
      FPDF_CloseDocument(doc);

      auto start = std::chrono::high_resolution_clock::now();
      int result;
      if (workers > 1) {
        result = RenderMultiProcess(pdf_path, dpi, pattern, pages, workers, compression);
      } else {
        result = RenderSingleProcess(pdf_path, dpi, pattern, pages, compression);
      }
      auto end = std::chrono::high_resolution_clock::now();
      double elapsed = std::chrono::duration<double>(end - start).count();

      if (result == 0)
        printf("OK %d %.3f\n", pages, elapsed);
      else
        printf("ERROR render failed\n");
      fflush(stdout);
      continue;
    }

    printf("ERROR unknown command\n");
    fflush(stdout);
  }

  FPDF_DestroyLibrary();
  return 0;
}
#endif

int main(int argc, char* argv[]) {
  // --info: just print page count and exit (no rendering)
  if (argc == 3 && strcmp(argv[1], "--info") == 0) {
    FPDF_LIBRARY_CONFIG config;
    memset(&config, 0, sizeof(config));
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);

    FPDF_DOCUMENT doc = FPDF_LoadDocument(argv[2], nullptr);
    if (!doc) {
      fprintf(stderr, "Failed to open: %s\n", argv[2]);
      FPDF_DestroyLibrary();
      return 1;
    }
    printf("%d\n", FPDF_GetPageCount(doc));
    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return 0;
  }

  // --daemon: persistent worker mode (reads commands from stdin)
#ifndef _WIN32
  if (argc == 2 && strcmp(argv[1], "--daemon") == 0) {
    return RunDaemon();
  }
#endif

  if (argc < 3) {
    fprintf(stderr, "fastpdf2png - Ultra-fast PDF to PNG converter\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s input.pdf output_%%03d.png [dpi] [workers] [-c level]\n", argv[0]);
    fprintf(stderr, "  %s --info input.pdf              # print page count\n", argv[0]);
    fprintf(stderr, "  %s --daemon                      # persistent worker mode\n", argv[0]);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  dpi       Resolution (default: 300)\n");
    fprintf(stderr, "  workers   Parallel workers (default: 1)\n");
    fprintf(stderr, "  -c level  0=fast, 1=medium, 2=best (default: 0)\n");
    return 1;
  }

  const char* pdf_path = argv[1];
  const char* pattern = argv[2];
  float dpi = (argc > 3) ? static_cast<float>(atof(argv[3])) : 300.0f;
  int workers = (argc > 4) ? atoi(argv[4]) : 1;
  int compression = 0;

  for (int i = 5; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      compression = atoi(argv[i + 1]);
      if (compression < 0) compression = 0;
      if (compression > 2) compression = 2;
      break;
    }
  }

  if (dpi <= 0 || dpi > 2400) { fprintf(stderr, "DPI must be 1-2400\n"); return 1; }
  if (workers < 1) workers = 1;
  if (workers > 64) workers = 64;

  FPDF_LIBRARY_CONFIG config;
  memset(&config, 0, sizeof(config));
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);

  FPDF_DOCUMENT doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) {
    fprintf(stderr, "Failed to open PDF: %s (error %lu)\n", pdf_path, FPDF_GetLastError());
    FPDF_DestroyLibrary();
    return 1;
  }

  int pages = FPDF_GetPageCount(doc);
  FPDF_CloseDocument(doc);

  if (pages <= 0) {
    fprintf(stderr, "PDF has no pages\n");
    FPDF_DestroyLibrary();
    return 1;
  }

  auto start = std::chrono::high_resolution_clock::now();

  int result;
#ifndef _WIN32
  if (workers > 1) {
    result = RenderMultiProcess(pdf_path, dpi, pattern, pages, workers, compression);
  } else {
    result = RenderSingleProcess(pdf_path, dpi, pattern, pages, compression);
  }
#else
  result = RenderSingleProcess(pdf_path, dpi, pattern, pages, compression);
#endif
  FPDF_DestroyLibrary();

  auto end = std::chrono::high_resolution_clock::now();
  double elapsed = std::chrono::duration<double>(end - start).count();

  if (result == 0)
    printf("Rendered %d pages in %.3f seconds (%.1f pages/sec)\n",
           pages, elapsed, pages / elapsed);

  return result;
}
