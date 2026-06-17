#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"

# 1. Python environment (Pillow + pillow-heif). This path also works on Windows.
"${PYTHON_BIN}" -m venv "${ROOT_DIR}/.venv"
"${ROOT_DIR}/.venv/bin/python" -m pip install --upgrade pip
"${ROOT_DIR}/.venv/bin/python" -m pip install -r "${ROOT_DIR}/requirements.txt"

# 2. Native build dependencies for the faster C version (macOS / Linux).
if command -v brew >/dev/null 2>&1; then
    brew install cmake libheif webp ffmpeg
elif command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y build-essential cmake libheif-dev libwebp-dev ffmpeg
else
    echo "No supported package manager found." >&2
    echo "Install cmake, libheif, libwebp, and ffmpeg manually, then re-run." >&2
fi

# 3. Build the C executables into build/.
cmake -B "${ROOT_DIR}/build" -DCMAKE_BUILD_TYPE=Release "${ROOT_DIR}"
cmake --build "${ROOT_DIR}/build" -j

cat <<'MSG'
Setup complete.

C version (fast, macOS / Linux):
  ./build/collage --out collage.png --width 6480
  ./build/video --image collage.png --out tour.mp4 --format reel --duration 50
  ./build/collage --out collage.png --width 6480 --video --video-out tour.mp4

Python version (also works on Windows):
  .venv/bin/python collage.py --out collage.png --width 6480
  .venv/bin/python video.py --image collage.png --out tour.mp4 --format reel --duration 50
  .venv/bin/python collage.py --out collage.png --width 6480 --video --video-out tour.mp4
MSG
