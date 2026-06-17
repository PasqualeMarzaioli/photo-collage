# Collage

Beginner-friendly guide for creating:

1. A vertical 9:16 Instagram Stories collage from all photos in `photos/`.
2. A vertical Reel-style zoom video from that collage.

The project runs from the terminal. Every source photo is used exactly once.

## What You Need

Install these three things before starting:

- **Git**, used to download the project.
- **Python 3.11 or newer**, used to run the scripts.
- **ffmpeg**, used to create the MP4 video.

If you only want the image collage, ffmpeg is not needed. If you want the video,
ffmpeg is required.

## How To Read This Guide

- Open the terminal for your operating system.
- Copy one command block at a time.
- Paste it into the terminal.
- Press `Enter`.
- Wait until the command finishes before running the next one.

Do not copy section titles such as `macOS / Linux` or `Windows PowerShell`.
Only copy the text inside the command boxes.

## Check What Is Already Installed

### macOS / Linux

```bash
git --version
python3 --version
ffmpeg -version
```

### Windows PowerShell

```powershell
git --version
py --version
ffmpeg -version
```

If one of these commands says the program was not found, install that program
before continuing.

## Install Missing Programs

Use only the section for your operating system.

### macOS

The easiest way on macOS is Homebrew.

First check if Homebrew is installed:

```bash
brew --version
```

If `brew` is not found, install Homebrew from:

```text
https://brew.sh
```

After Homebrew is installed, install Git, Python, and ffmpeg:

```bash
brew install git python ffmpeg
```

Then check again:

```bash
git --version
python3 --version
ffmpeg -version
```

### Linux

On Ubuntu or Debian, run:

```bash
sudo apt update
sudo apt install git python3 python3-venv python3-pip ffmpeg
```

Then check again:

```bash
git --version
python3 --version
ffmpeg -version
```

If you use a different Linux distribution, install the same programs with your
distribution's package manager.

### Windows

On Windows, the easiest way is usually `winget`, which is included in recent
versions of Windows 10 and Windows 11.

Check if `winget` is available:

```powershell
winget --version
```

If `winget` works, install Git, Python, and ffmpeg:

```powershell
winget install --id Git.Git -e
winget install --id Python.Python.3.12 -e
winget install --id Gyan.FFmpeg -e
```

Close PowerShell, open it again, then check:

```powershell
git --version
py --version
ffmpeg -version
```

If `winget` is not available, install them manually from:

```text
https://git-scm.com/download/win
https://www.python.org/downloads/windows/
https://ffmpeg.org/download.html
```

When installing Python manually on Windows, enable the option named **Add Python
to PATH** if the installer shows it.

## Download The Project

Open a terminal first:

- **macOS:** open `Terminal`.
- **Linux:** open your terminal app.
- **Windows:** open `PowerShell`.

Choose a folder where you want to keep the project, then run:

```bash
git clone https://github.com/PasqualeMarzaioli/photo-mosaic.git collage
cd collage
```

If you downloaded the project as a ZIP instead of using Git, unzip it, open the
project folder, and continue from the setup step below.

If the project is already on your computer, skip `git clone` and use `cd` to go
inside the existing project folder.

## Add Your Photos

Put all source photos inside the `photos/` folder.

Supported formats:

- `.jpg` / `.jpeg`
- `.png`
- `.webp`
- `.heic` / `.heif`

## Setup On macOS

From inside the project folder:

```bash
bash setup.sh
```

Check that ffmpeg is installed:

```bash
ffmpeg -version
```

If macOS says ffmpeg is not found, install it with Homebrew:

```bash
brew install ffmpeg
```

Then check again:

```bash
ffmpeg -version
```

## Setup On Linux

From inside the project folder:

```bash
bash setup.sh
```

Check that ffmpeg is installed:

```bash
ffmpeg -version
```

On Ubuntu or Debian, install ffmpeg with:

```bash
sudo apt update
sudo apt install ffmpeg
```

Then check again:

```bash
ffmpeg -version
```

## Setup On Windows

Open `PowerShell` inside the project folder.

Create the Python environment:

```powershell
py -m venv .venv
```

Install the required Python packages:

```powershell
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

Check that ffmpeg is installed:

```powershell
ffmpeg -version
```

If PowerShell says ffmpeg is not found, install ffmpeg for Windows and add it to
your `PATH`. After installing, close PowerShell, open it again, go back to the
project folder, and run:

```powershell
ffmpeg -version
```

If you do not want to add ffmpeg to `PATH`, you can pass the full path later:

```powershell
--ffmpeg "C:\ffmpeg\bin\ffmpeg.exe"
```

## Create A Quick Test Collage

Use this first. It is faster and lets you check that everything works.

### macOS / Linux

```bash
.venv/bin/python collage.py --out collage-test.png --width 1080
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe collage.py --out collage-test.png --width 1080
```

The result will be `collage-test.png`.

## Create The Final Collage

This creates a high-resolution 9:16 image.

### macOS / Linux

```bash
.venv/bin/python collage.py --out collage.png --width 6480
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe collage.py --out collage.png --width 6480
```

The result will be `collage.png`.

The output size will be:

- `--width 1080` -> `1080 x 1920`
- `--width 2160` -> `2160 x 3840`
- `--width 6480` -> `6480 x 11520`

## Create The Video

Create `collage.png` first, then run the video command.

### macOS / Linux

```bash
.venv/bin/python video.py --image collage.png --out tour.mp4 \
  --format reel --duration 50 --fps 30 --cycles 4 --zoom 2.8
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe video.py --image collage.png --out tour.mp4 --format reel --duration 50 --fps 30 --cycles 4 --zoom 2.8
```

The result will be `tour.mp4`.

The video opens on the full collage, zooms in once, pans across the photos at a
steady zoom, then slowly zooms back out at the end.

## Create Collage And Video In One Command

This creates both `collage.png` and `tour.mp4`.

### macOS / Linux

```bash
.venv/bin/python collage.py --out collage.png --width 6480 \
  --video --video-out tour.mp4
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe collage.py --out collage.png --width 6480 --video --video-out tour.mp4
```

## Useful Options

Change the approximate number of columns:

```bash
--cols 14
```

Shuffle the photo order in a repeatable way:

```bash
--shuffle --seed 1
```

Add a small gap between photos:

```bash
--gap 2
```

Round the photo corners:

```bash
--radius 8
```

The default is connected photos:

```bash
--gap 0 --radius 0
```

## Common Problems

### `No images found in photos/`

The `photos/` folder is empty, or the files are not supported images. Put your
photos inside `photos/` and try again.

### `HEIC support requires pillow-heif`

Run setup again:

macOS / Linux:

```bash
bash setup.sh
```

Windows:

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

### `ffmpeg not found`

ffmpeg is required for the video. Install ffmpeg, or pass the full ffmpeg path
with `--ffmpeg`.

### The video looks blurry when zoomed in

Render the collage larger first:

```bash
.venv/bin/python collage.py --out collage.png --width 6480
```

Then create the video again.

### The command is slow

This is normal with many HEIC photos or with `--width 6480`. Use the quick test
command first, then run the final high-resolution command.
