#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="$ROOT_DIR/build/fastpdf2png"
BENCH_DIR="/tmp/fastpdf2png_bench"
SAMPLE_PDF="$ROOT_DIR/bench_sample.pdf"

# Use user-provided PDF or download a sample
if [ -n "$1" ]; then
    SAMPLE_PDF="$1"
elif [ ! -f "$SAMPLE_PDF" ]; then
    echo "Downloading sample PDF (arxiv survey, ~35 pages, mixed content)..."
    curl -L --fail -o "$SAMPLE_PDF" "https://arxiv.org/pdf/2312.10997" 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "Download failed. Provide your own PDF:"
        echo "  bash scripts/benchmark.sh /path/to/your.pdf"
        exit 1
    fi
fi

if [ ! -f "$SAMPLE_PDF" ]; then
    echo "Error: PDF not found: $SAMPLE_PDF"
    exit 1
fi

# Build if needed
if [ ! -f "$BINARY" ]; then
    echo "Binary not found, building..."
    bash "$SCRIPT_DIR/build.sh"
fi

# Machine info
echo "=== Machine ==="
OS=$(uname -s)
ARCH=$(uname -m)
if [ "$OS" = "Darwin" ]; then
    CPU=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")
    RAM=$(sysctl -n hw.memsize 2>/dev/null | awk '{printf "%.0f GB", $0/1024/1024/1024}')
else
    CPU=$(grep -m1 "model name" /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo "unknown")
    RAM=$(free -h 2>/dev/null | awk '/Mem:/{print $2}' || echo "unknown")
fi
echo "CPU: $CPU"
echo "RAM: $RAM"
echo "OS:  $OS $ARCH"
echo ""

# Get page count from first run
echo "=== Document ==="
echo "File: $(basename "$SAMPLE_PDF")"
echo "Size: $(du -h "$SAMPLE_PDF" | cut -f1 | xargs)"
echo ""

# Benchmark
echo "=== Benchmark (150 DPI, compression=2/best) ==="
rm -rf "$BENCH_DIR"
mkdir -p "$BENCH_DIR"

echo ""
echo "--- 1 worker ---"
for i in 1 2 3; do
    rm -rf "$BENCH_DIR"/*
    "$BINARY" "$SAMPLE_PDF" "$BENCH_DIR/page_%03d.png" 150 1 -c 2 2>&1 | grep "Rendered"
done

echo ""
echo "--- 4 workers ---"
for i in 1 2 3; do
    rm -rf "$BENCH_DIR"/*
    "$BINARY" "$SAMPLE_PDF" "$BENCH_DIR/page_%03d.png" 150 4 -c 2 2>&1 | grep "Rendered"
done

echo ""
echo "--- 8 workers ---"
for i in 1 2 3 4 5; do
    rm -rf "$BENCH_DIR"/*
    "$BINARY" "$SAMPLE_PDF" "$BENCH_DIR/page_%03d.png" 150 8 -c 2 2>&1 | grep "Rendered"
done

echo ""
echo "=== Output ==="
echo "Total size: $(du -sh "$BENCH_DIR" | cut -f1 | xargs)"
echo "Pages: $(ls "$BENCH_DIR"/*.png 2>/dev/null | wc -l | xargs)"

# Count grayscale vs RGB
GRAY=0
RGB=0
for f in "$BENCH_DIR"/*.png; do
    CT=$(python3 -c "
import struct
with open('$f','rb') as f:
    f.read(16); d=f.read(13)
    print(d[9])
" 2>/dev/null || echo "?")
    if [ "$CT" = "0" ]; then
        GRAY=$((GRAY + 1))
    elif [ "$CT" = "2" ]; then
        RGB=$((RGB + 1))
    fi
done
echo "Grayscale pages: $GRAY"
echo "Color (RGB) pages: $RGB"

# Cleanup
rm -rf "$BENCH_DIR"
echo ""
echo "Done."
