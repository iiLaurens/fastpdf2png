#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Detect OS and architecture
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)

case "$OS" in
    linux)
        case "$ARCH" in
            x86_64) PLATFORM="linux-x64" ;;
            aarch64|arm64) PLATFORM="linux-arm64" ;;
            *) echo "Unsupported arch: $ARCH"; exit 1 ;;
        esac
        ;;
    darwin)
        case "$ARCH" in
            x86_64) PLATFORM="mac-x64" ;;
            arm64) PLATFORM="mac-arm64" ;;
            *) echo "Unsupported arch: $ARCH"; exit 1 ;;
        esac
        ;;
    *)
        echo "Unsupported OS: $OS"
        exit 1
        ;;
esac

echo "=== Downloading pre-built PDFium for $PLATFORM ==="

# Use bblanchon's pdfium-binaries (well maintained)
# https://github.com/bblanchon/pdfium-binaries/releases
PDFIUM_BUILD="7713"
DOWNLOAD_URL="https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F${PDFIUM_BUILD}/pdfium-${PLATFORM}.tgz"

echo "Trying primary source..."
mkdir -p "$ROOT_DIR/pdfium"
cd "$ROOT_DIR/pdfium"

# Try primary source
if curl -L --fail -o pdfium.tgz "$DOWNLOAD_URL" 2>/dev/null; then
    echo "Downloaded from primary source"
else
    # Fallback to alternative source (latest release)
    echo "Primary source failed, trying latest release..."
    DOWNLOAD_URL="https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-${PLATFORM}.tgz"

    if curl -L --fail -o pdfium.tgz "$DOWNLOAD_URL" 2>/dev/null; then
        echo "Downloaded from alternative source"
    else
        echo ""
        echo "ERROR: Could not download pre-built PDFium"
        echo ""
        echo "Download manually from:"
        echo "  https://github.com/bblanchon/pdfium-binaries/releases"
        echo ""
        echo "Extract into the 'pdfium/' directory at the project root."
        exit 1
    fi
fi

# Check if it's a valid archive
if ! tar tzf pdfium.tgz >/dev/null 2>&1; then
    echo "Downloaded file is not a valid archive"
    echo "Content: $(head -c 100 pdfium.tgz)"
    rm -f pdfium.tgz
    exit 1
fi

echo "Extracting..."
tar xzf pdfium.tgz
rm pdfium.tgz

echo ""
echo "=== PDFium downloaded ==="
ls -la "$ROOT_DIR/pdfium"
