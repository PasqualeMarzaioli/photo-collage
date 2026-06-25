# Collage

Beginner-friendly guide for creating:

1. A vertical 9:16 collage from all photos in `photos/`.
2. A vertical zoom video from that collage.

The project runs from the terminal. Every source photo is used exactly once.

## Two Versions: Python and C

This toolkit comes in two versions that do exactly the same thing and produce
the same result:

- **Python version** — works on every operating system. **Windows users must use
  this version.**
- **C version** — a faster build for **macOS and Linux only**. It uses all of your
  computer's CPU cores, so large collages and videos finish sooner. It does
  **not** run on Windows.

What to do:

- **On Windows:** follow the Python instructions below and skip the C section.
- **On macOS or Linux:** you can use either one. Start with Python if you just
  want it to work; switch to the C version (the section "Faster Version In C")
  when you want it to be faster.

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
git clone https://github.com/PasqualeMarzaioli/photo-collage collage
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

## Add Your Audio (Optional)

If you want the video to have a soundtrack, put your audio files inside the
`audio/` folder.

Use one file as the main track and one file as the background track:

- Main track: voice-over, speech, or the audio that should stay in front.
- Background track: music or ambience that should play quietly under the main
  track, then rise after the main track ends.

For example:

```text
audio/audio.m4a
audio/background.mov
```

The exact file names are your choice. Use the same paths in the command:

```bash
--audio-main audio/audio.m4a --audio-bg audio/background.mov
```

Audio is optional. If you do not pass `--audio-main` or `--audio-bg`, the video
is silent, exactly like the original version of this tool.

When both tracks are present, the background starts immediately. By default, the
main track is centered inside the background track:

```text
main start = (background duration - main duration) / 2
```

For example, with a 2-minute background and a 1-minute 30-second main track, the
main track starts after 15 seconds. To choose the start time yourself, add:

```bash
--audio-main-start 15
```

Local audio files are ignored by git, so you can keep personal tracks in
`audio/` without accidentally committing them.

## Choose Your Version (Python or C)

Everything up to this point — installing the tools, downloading the project, and
adding your photos — is the same for everyone. **From here the guide splits in
two. Follow only one path:**

- 🐍 **To use the Python version** (works everywhere, and the only option on
  **Windows**): **keep reading the next section** ("Setup On macOS" / "Setup On
  Linux" / "Setup On Windows") and continue straight down the guide.
- ⚡ **To use the faster C version** (**macOS and Linux only**): **skip ahead** to
  the section **"Faster Version In C (macOS and Linux)"** further down, and follow
  it from there.

You can always come back later and try the other version; the photos and project
folder are shared.

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
  --format reel --duration 50 --fps 30 --zoom 2.8
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe video.py --image collage.png --out tour.mp4 --format reel --duration 50 --fps 30 --zoom 2.8
```

The result will be `tour.mp4`.

By default the video opens on the full collage, zooms in once, then moves across
the photos to show as many of them as possible (the `cover` tour), and finally
zooms back out at the end.

The video is silent unless you add audio flags. To mix a main track with a
background song, pass both paths. The background loops, stays quiet under the
main track, then rises after the main track ends:

### macOS / Linux

```bash
.venv/bin/python video.py --image collage.png --out tour-audio.mp4 \
  --format reel --duration 50 \
  --audio-main audio/audio.m4a --audio-bg audio/background.mov
```

To force the main track to start after a specific number of seconds, add
`--audio-main-start`:

```bash
.venv/bin/python video.py --image collage.png --out tour-audio.mp4 \
  --format reel --duration 50 \
  --audio-main audio/audio.m4a --audio-bg audio/background.mov \
  --audio-main-start 15
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe video.py --image collage.png --out tour-audio.mp4 --format reel --duration 50 --audio-main audio\audio.m4a --audio-bg audio\background.mov
```

To create a two-minute video, set `--duration 120`:

### macOS / Linux

```bash
.venv/bin/python video.py --image collage.png --out tour-2min.mp4 \
  --format reel --duration 120 \
  --audio-main audio/audio.m4a --audio-bg audio/background.mov
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe video.py --image collage.png --out tour-2min.mp4 --format reel --duration 120 --audio-main audio\audio.m4a --audio-bg audio\background.mov
```

To make the camera move faster between photos, add `--pan-speed` (for example
`--pan-speed 2` for twice as fast). To go back to the older, calmer path that
visits only a few areas, add `--tour classic` (with `classic` you can also set
`--cycles` to change how many areas it visits). Full list of options is in
"All Options (Reference)" below.

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

To create the collage and a two-minute video with audio in one command:

### macOS / Linux

```bash
.venv/bin/python collage.py --out collage.png --width 6480 \
  --video --video-out tour-2min.mp4 \
  --duration 120 \
  --audio-main audio/audio.m4a --audio-bg audio/background.mov
```

### Windows PowerShell

```powershell
.\.venv\Scripts\python.exe collage.py --out collage.png --width 6480 --video --video-out tour-2min.mp4 --duration 120 --audio-main audio\audio.m4a --audio-bg audio\background.mov
```

## Faster Version In C (macOS and Linux)

*This is the section the "Choose Your Version" signpost points to. You do not
need the Python setup sections above to use it — only the common steps (install
the tools, download the project, add your photos).*

The project also ships a version written in C. It does the same thing as the
Python scripts and produces the same images and videos, but it is faster because
it spreads the work across all of your CPU cores.

**This version runs only on macOS and Linux. It cannot be built or run on
Windows** (it uses POSIX features that Windows does not provide). On Windows,
use the Python version above.

### What The C Version Needs

The C version is compiled from source, so you need a few tools and libraries:

- A **C compiler** (Clang on macOS, GCC on Linux).
- **CMake**, the tool that configures and runs the build.
- **libheif**, to read iPhone `.heic` photos.
- **libwebp**, to read and write `.webp` images.
- **ffmpeg**, to encode the video (same as the Python version).

The two single-header `stb` libraries used for JPEG and PNG are already included
in the `third_party/` folder, so there is nothing to download for those.

### Install The Tools

Use only the section for your operating system.

#### macOS

The compiler comes with the Xcode Command Line Tools. Install them once:

```bash
xcode-select --install
```

The other tools are installed with Homebrew. If you do not have Homebrew yet,
install it from:

```text
https://brew.sh
```

Then install CMake and the libraries:

```bash
brew install cmake libheif webp ffmpeg
```

#### Linux (Ubuntu or Debian)

```bash
sudo apt update
sudo apt install build-essential cmake libheif-dev libwebp-dev ffmpeg
```

On a different Linux distribution, install the same packages with your
distribution's package manager (the development packages, e.g. `libheif-dev`).

### Build The C Programs

The easiest way is the setup script, which installs the libraries (on macOS or
Ubuntu/Debian), creates the Python environment, and builds the C programs all at
once. From inside the project folder:

```bash
bash setup.sh
```

If you prefer to build manually after the libraries are installed, run these two
commands from inside the project folder:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j
```

The first command configures the build inside a new `build/` folder. The second
command compiles it. When it finishes, you will have two programs:

- `build/collage`
- `build/video`

Check that they were built:

```bash
./build/collage --help
./build/video --help
```

If you see the list of options, the build worked.

### Run The C Programs

The C programs take exactly the same options as the Python scripts. The only
difference is how you start them: you call the program in the `build/` folder
directly, with no `python` in front.

Create a quick test collage (fast, for checking it works):

```bash
./build/collage --out collage-test.png --width 1080
```

Create the final high-resolution collage:

```bash
./build/collage --out collage.png --width 6480
```

Create the video from an existing collage:

```bash
./build/video --image collage.png --out tour.mp4 \
  --format reel --duration 50 --fps 30 --zoom 2.8
```

Create a video with a main track and a looping background song:

```bash
./build/video --image collage.png --out tour-audio.mp4 \
  --format reel --duration 50 \
  --audio-main audio/audio.m4a --audio-bg audio/background.mov
```

Create a two-minute video with audio:

```bash
./build/video --image collage.png --out tour-2min.mp4 \
  --format reel --duration 120 \
  --audio-main audio/audio.m4a --audio-bg audio/background.mov
```

Create the collage and the video in one command:

```bash
./build/collage --out collage.png --width 6480 --video --video-out tour.mp4
```

Create the collage and a two-minute video with audio in one command:

```bash
./build/collage --out collage.png --width 6480 \
  --video --video-out tour-2min.mp4 \
  --duration 120 \
  --audio-main audio/audio.m4a --audio-bg audio/background.mov
```

All the options described in "Useful Options" below work the same way for the C
programs.

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

## All Options (Reference)

These options work the same way in both versions. Just add them after the
command, for example:

```bash
.venv/bin/python video.py --image collage.png --out tour.mp4 --fps 60 --pan-speed 2
./build/video --image collage.png --out tour.mp4 --fps 60 --pan-speed 2
```

### Collage options (`collage.py` / `build/collage`)

| Option | What it does | Default |
|---|---|---|
| `--photos DIR` | Folder with the source photos | `photos` |
| `--out PATH` | Output image path | `collage.png` |
| `--width N` | Output width in pixels (height is 16/9 of the width) | `2160` |
| `--scale F` | Output scale relative to 1080 px width (alternative to `--width`) | — |
| `--cols N` | Approximate number of columns (omit for automatic) | auto |
| `--gap N` | Gap between photos, in base 1080 px units | `0` |
| `--radius N` | Rounded-corner radius, in base 1080 px units | `0` |
| `--bg COLOR` | Background color, e.g. `#ffffff` | `#ffffff` |
| `--shuffle` | Shuffle the photo order | off |
| `--seed N` | Seed for `--shuffle` (same seed = same order) | `0` |
| `--video` | Also render the video right after the collage | off |
| `--video-out PATH` | Output path when using `--video` | `tour.mp4` |
| `--ffmpeg PATH` | ffmpeg binary name or full path | `ffmpeg` |

### Video options (`video.py` / `build/video`)

| Option | What it does | Default |
|---|---|---|
| `--image PATH` | Source collage image (**required**) | — |
| `--out PATH` | Output MP4 path | `tour.mp4` |
| `--format NAME` | `reel` (1080×1920), `feed` (1080×1350), or `square` (1080×1080) | `reel` |
| `--duration S` | Video length in seconds | `50` |
| `--fps N` | Frames per second (1–120) | `30` |
| `--zoom F` | Maximum zoom (≥ 1.0) | `2.8` |
| `--tour NAME` | `cover` (moves over all photos) or `classic` (sparse wander) | `cover` |
| `--pan-speed F` | Camera speed between photos (> 0; `2` = twice as fast) | `1.0` |
| `--cycles N` | Number of pan segments — **classic tour only** | `4` |
| `--crf N` | H.264 quality, 0–51, lower = better | `18` |
| `--preset NAME` | x264 preset, e.g. `fast`, `medium`, `slow` | `medium` |
| `--ffmpeg PATH` | ffmpeg binary name or full path | `ffmpeg` |
| `--audio-main PATH` | Main audio track; enables audio when set | — |
| `--audio-bg PATH` | Background song; enables audio when set | — |
| `--audio-main-vol F` | Main track gain | `1.0` |
| `--audio-bg-low F` | Background gain while the main track plays | `0.15` |
| `--audio-bg-high F` | Background gain after the main track ends | `1.0` |
| `--audio-fade F` | Rise and ending fade length in seconds | `1.5` |
| `--audio-main-start S` | Main track start time in seconds; omit for automatic centering | auto |

When you generate the video together with the collage (`collage … --video`), the
video tuning options `--duration`, `--fps`, `--cycles`, `--zoom`, `--tour`, and
`--pan-speed` are accepted too, along with the audio options above. `--format`,
`--crf`, and `--preset` are available only on the standalone `video` program
(the combined command always uses `reel`, crf 18, preset medium).

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

### `ffprobe not found`

ffprobe is required when `--audio-main` is used so the background rise can start
when the main track ends. It normally installs together with ffmpeg. If needed,
pass an ffmpeg path whose folder also contains ffprobe.

### The video looks blurry when zoomed in

Render the collage larger first:

```bash
.venv/bin/python collage.py --out collage.png --width 6480
```

Then create the video again.

### The command is slow

This is normal with many HEIC photos or with `--width 6480`. Use the quick test
command first, then run the final high-resolution command.
