#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"

"${PYTHON_BIN}" -m venv "${ROOT_DIR}/.venv"
"${ROOT_DIR}/.venv/bin/python" -m pip install --upgrade pip
"${ROOT_DIR}/.venv/bin/python" -m pip install -r "${ROOT_DIR}/requirements.txt"

cat <<'MSG'
Setup complete.

Use:
  .venv/bin/python collage.py --out collage.png --width 6480
  .venv/bin/python video.py --image collage.png --out tour.mp4 --format reel --duration 50
  .venv/bin/python collage.py --out collage.png --width 6480 --video --video-out tour.mp4
MSG
