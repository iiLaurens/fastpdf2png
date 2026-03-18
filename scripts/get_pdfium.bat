@echo off
setlocal

set SCRIPT_DIR=%~dp0
set ROOT_DIR=%SCRIPT_DIR%..

echo === Downloading pre-built PDFium for win-x64 ===

set PDFIUM_BUILD=6721
set DOWNLOAD_URL=https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/%PDFIUM_BUILD%/pdfium-win-x64.tgz

if not exist "%ROOT_DIR%\pdfium" mkdir "%ROOT_DIR%\pdfium"
cd "%ROOT_DIR%\pdfium"

echo Trying primary source...
curl -L --fail -o pdfium.tgz "%DOWNLOAD_URL%" 2>nul
if errorlevel 1 (
    echo Primary source failed, trying latest release...
    curl -L --fail -o pdfium.tgz "https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-win-x64.tgz" 2>nul
    if errorlevel 1 (
        echo ERROR: Could not download pre-built PDFium
        echo Download manually from: https://github.com/bblanchon/pdfium-binaries/releases
        exit /b 1
    )
)

echo Extracting...
tar xzf pdfium.tgz
del pdfium.tgz

echo === PDFium downloaded ===
