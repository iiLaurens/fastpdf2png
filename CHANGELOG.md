# Changelog

## 1.0.0

- Initial release
- PDFium-based rendering with SIMD-optimized PNG encoding (NEON, AVX2, SSE)
- Auto grayscale detection for smaller output files
- fork()-based parallelism with shared-memory work stealing
- Python SDK: `to_images()`, `to_files()`, `to_bytes()`, `page_count()`, `Engine`
- Node.js SDK: `toFiles()`, `toBuffers()`, `pageCount()`, `Engine`
- CLI with daemon mode for batch processing
- CI/CD: builds for macOS (arm64, x86_64), Linux (x86_64, arm64)
- Publishes to PyPI and npm on tag
