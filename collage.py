#!/usr/bin/env python3
"""Create a collage from local photos.

This program reads supported images from the photos folder, places each photo
exactly once into a connected vertical collage, and can optionally render a
zoom-tour video from the generated collage.

Author: Pasquale Marzaioli
    Pasquale Marzaioli
"""

from __future__ import annotations

import argparse
import math
import random
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image, ImageDraw, ImageOps
except ImportError as exc:
    raise SystemExit(
        "Pillow is required. Run `bash setup.sh` from this directory first."
    ) from exc

try:
    import pillow_heif

    pillow_heif.register_heif_opener()
    HEIF_ENABLED = True
except ImportError:
    HEIF_ENABLED = False


BASE_WIDTH = 1080
BASE_HEIGHT = 1920
TILE_ASPECT = 9 / 16
DEFAULT_WIDTH = 2160
SUPPORTED_EXTENSIONS = {
    ".jpg",
    ".jpeg",
    ".png",
    ".webp",
    ".heic",
    ".heif",
}


class CollageError(Exception):
    """Raised when collage rendering cannot continue with the given inputs."""


@dataclass(frozen=True)
class Layout:
    """Store final canvas and grid measurements in output pixels."""

    width: int
    height: int
    cols: int
    rows: int
    row_counts: tuple[int, ...]
    gap: int
    radius: int
    background: tuple[int, int, int]


@dataclass(frozen=True)
class CollageOptions:
    """Store all settings needed to render a collage and optional video."""

    photos_dir: Path
    out: Path
    cols: int | None
    width: int
    radius: int
    gap: int
    background: tuple[int, int, int]
    shuffle: bool
    seed: int
    video: bool
    video_out: Path
    video_duration: float
    video_fps: int
    video_cycles: int
    video_zoom: float
    video_tour: str
    video_pan_speed: float
    ffmpeg: str
    audio_main: Path | None
    audio_bg: Path | None
    audio_main_vol: float
    audio_bg_low: float
    audio_bg_high: float
    audio_fade: float


def parse_color(value: str | Iterable[int]) -> tuple[int, int, int]:
    """Parse a hex string or RGB iterable into an RGB tuple."""

    if isinstance(value, str):
        text = value.strip()
        if text.startswith("#"):
            text = text[1:]
        if len(text) == 3:
            text = "".join(channel * 2 for channel in text)
        if len(text) != 6:
            raise CollageError(
                f"Invalid color `{value}`. Use a value like #ffffff.")
        try:
            return (
                int(text[0:2], 16),
                int(text[2:4], 16),
                int(text[4:6], 16),
            )
        except ValueError as exc:
            raise CollageError(
                f"Invalid color `{value}`. Use a value like #ffffff.") from exc

    parts = list(value)
    if len(parts) != 3:
        raise CollageError("RGB colors must contain exactly three channels.")
    channels = tuple(int(channel) for channel in parts)
    if any(channel < 0 or channel > 255 for channel in channels):
        raise CollageError("RGB color channels must be between 0 and 255.")
    return channels


def choose_auto_columns(photo_count: int) -> int:
    """Choose a column count that keeps cells close to 9:16 story tiles."""

    if photo_count <= 0:
        raise CollageError("At least one photo is required.")

    best_cols = 1
    best_score = float("inf")
    for cols in range(1, photo_count + 1):
        rows = max(1, round(photo_count / cols))
        cell_ratio = (BASE_WIDTH / cols) / (BASE_HEIGHT / rows)
        score = abs(math.log(cell_ratio / TILE_ASPECT))
        if score < best_score:
            best_score = score
            best_cols = cols
    return best_cols


def distribute_row_counts(photo_count: int, desired_cols: int) -> tuple[int, ...]:
    """Distribute all photos across balanced rows with no repeated or blank cells."""

    if photo_count <= 0:
        raise CollageError("At least one photo is required.")
    if desired_cols <= 0:
        raise CollageError("--cols must be greater than zero.")

    rows = max(1, round(photo_count / desired_cols))
    row_counts = tuple(
        math.floor((row + 1) * photo_count / rows) -
        math.floor(row * photo_count / rows)
        for row in range(rows)
    )
    if any(count <= 0 for count in row_counts):
        raise CollageError("Could not create a valid no-repeat row layout.")
    return row_counts


def build_layout(photo_count: int, options: CollageOptions) -> Layout:
    """Build final canvas and no-repeat row layout from render options."""

    base_cols = options.cols or choose_auto_columns(photo_count)
    row_counts = distribute_row_counts(photo_count, base_cols)
    width = options.width
    if width <= 0:
        raise CollageError("--width must be greater than zero.")

    height = round(width * BASE_HEIGHT / BASE_WIDTH)
    scale = width / BASE_WIDTH
    gap = max(0, round(options.gap * scale))
    radius = max(0, round(options.radius * scale))
    cols = max(row_counts)
    rows = len(row_counts)

    if width - gap * (cols + 1) < cols or height - gap * (rows + 1) < rows:
        raise CollageError("Gap is too large for the selected width and grid.")

    return Layout(
        width=width,
        height=height,
        cols=cols,
        rows=rows,
        row_counts=row_counts,
        gap=gap,
        radius=radius,
        background=options.background,
    )


def collect_image_paths(directory: Path) -> list[Path]:
    """Return supported image files from a directory in stable name order."""

    if not directory.exists():
        raise CollageError(f"Photos directory not found: {directory}")
    if not directory.is_dir():
        raise CollageError(f"Photos path is not a directory: {directory}")

    paths = [
        path
        for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS
    ]
    paths.sort(key=lambda path: path.name.lower())
    if not paths:
        supported = ", ".join(sorted(extension.lstrip(".")
                              for extension in SUPPORTED_EXTENSIONS))
        raise CollageError(
            f"No images found in {directory}/ (supported: {supported})")
    return paths


def open_rgb_image(path: Path) -> Image.Image:
    """Open an image path with EXIF orientation applied and return RGB pixels."""

    if not path.is_file():
        raise CollageError(f"Image not found: {path}")
    if path.suffix.lower() in {".heic", ".heif"} and not HEIF_ENABLED:
        raise CollageError(
            "HEIC support requires pillow-heif. Run `bash setup.sh` and try again."
        )
    try:
        with Image.open(path) as image:
            return ImageOps.exif_transpose(image).convert("RGB")
    except OSError as exc:
        raise CollageError(f"Could not open image: {path}") from exc


def tile_box(layout: Layout, cell_index: int) -> tuple[int, int, int, int]:
    """Return the pixel box for one no-repeat cell index inside the canvas."""

    remaining = cell_index
    row = 0
    for row_index, row_count in enumerate(layout.row_counts):
        if remaining < row_count:
            row = row_index
            break
        remaining -= row_count
    else:
        raise CollageError(f"Cell index out of range: {cell_index}")

    col = remaining
    row_count = layout.row_counts[row]
    tile_width = (layout.width - layout.gap * (row_count + 1)) / row_count
    tile_height = (layout.height - layout.gap *
                   (layout.rows + 1)) / layout.rows
    left = round(layout.gap + col * (tile_width + layout.gap))
    top = round(layout.gap + row * (tile_height + layout.gap))
    right = round(layout.gap + col * (tile_width + layout.gap) + tile_width)
    bottom = round(layout.gap + row * (tile_height + layout.gap) + tile_height)
    return (left, top, max(left + 1, right), max(top + 1, bottom))


def rounded_mask(size: tuple[int, int], radius: int) -> Image.Image:
    """Create an alpha mask with optional rounded corners for one tile."""

    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle((0, 0, size[0], size[1]), radius=radius, fill=255)
    return mask


def prepare_tile(path: Path, size: tuple[int, int], radius: int) -> tuple[Image.Image, Image.Image]:
    """Crop and mask one source image for placement in a collage cell."""

    source = open_rgb_image(path)
    try:
        tile = ImageOps.fit(
            source,
            size,
            Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )
    finally:
        source.close()

    mask = rounded_mask(size, radius)
    return tile, mask


def render_collage_image(paths: list[Path], layout: Layout) -> Image.Image:
    """Render a simple no-repeat collage containing every source photo once."""

    canvas = Image.new("RGB", (layout.width, layout.height), layout.background)
    for cell_index, path in enumerate(paths):
        box = tile_box(layout, cell_index)
        size = (box[2] - box[0], box[3] - box[1])
        tile, mask = prepare_tile(path, size, layout.radius)
        canvas.paste(tile, box[:2], mask)
    return canvas


def save_image(image: Image.Image, path: Path) -> None:
    """Save a rendered image as PNG, JPEG, or WebP based on extension."""

    path.parent.mkdir(parents=True, exist_ok=True)
    suffix = path.suffix.lower()
    try:
        if suffix in {".jpg", ".jpeg"}:
            image.save(path, quality=95, subsampling=0, optimize=True)
        elif suffix == ".webp":
            image.save(path, quality=95, method=6)
        else:
            image.save(path)
    except OSError as exc:
        raise CollageError(f"Could not save output image: {path}") from exc


def render_collage(options: CollageOptions) -> Path:
    """Render the requested no-repeat collage and return the output image path."""

    paths = collect_image_paths(options.photos_dir)
    if options.shuffle:
        rng = random.Random(options.seed)
        rng.shuffle(paths)

    layout = build_layout(len(paths), options)
    image = render_collage_image(paths, layout)
    save_image(image, options.out)
    print(
        f"Rendered {len(paths)} photos as {layout.width}x{layout.height} "
        f"with {layout.rows} rows.",
        file=sys.stderr,
    )
    return options.out


def render_optional_video(options: CollageOptions, image_path: Path) -> None:
    """Render a zoom-tour video after the collage when requested."""

    if not options.video:
        return

    try:
        from video import VideoOptions, render_video

        render_video(
            VideoOptions(
                image=image_path,
                out=options.video_out,
                format="reel",
                duration=options.video_duration,
                fps=options.video_fps,
                cycles=options.video_cycles,
                zoom=options.video_zoom,
                ffmpeg=options.ffmpeg,
                crf=18,
                preset="medium",
                tour=options.video_tour,
                pan_speed=options.video_pan_speed,
                audio_main=options.audio_main,
                audio_bg=options.audio_bg,
                audio_main_vol=options.audio_main_vol,
                audio_bg_low=options.audio_bg_low,
                audio_bg_high=options.audio_bg_high,
                audio_fade=options.audio_fade,
            )
        )
    except Exception as exc:
        raise CollageError(f"Video rendering failed: {exc}") from exc


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments for collage rendering."""

    parser = argparse.ArgumentParser(
        description="Render a 9:16 Instagram Stories collage.")
    parser.add_argument("--photos", type=Path, default=Path("photos"),
                        help="Directory containing photos.")
    parser.add_argument("--out", type=Path,
                        default=Path("collage.png"), help="Output image path.")
    parser.add_argument(
        "--mode",
        choices=("collage", "grid"),
        default="collage",
        help="Render mode. `grid` is kept as an alias for collage.",
    )
    parser.add_argument("--cols", type=int,
                        help="Approximate column count, or omit for auto.")
    parser.add_argument("--scale", type=float,
                        help="Output scale relative to 1080 px width.")
    parser.add_argument("--width", type=int, default=None,
                        help="Output image width in pixels.")
    parser.add_argument("--radius", type=int, default=0,
                        help="Corner radius in base 1080 px units.")
    parser.add_argument("--gap", type=int, default=0,
                        help="Gap in base 1080 px units.")
    parser.add_argument("--bg", default="#ffffff",
                        help="Canvas background color, such as #ffffff.")
    parser.add_argument("--shuffle", action="store_true",
                        help="Shuffle source photos deterministically.")
    parser.add_argument("--seed", type=int, default=0, help="Shuffle seed.")
    parser.add_argument("--video", action="store_true",
                        help="Render a Reel MP4 after the collage.")
    parser.add_argument("--video-out", type=Path,
                        default=Path("tour.mp4"), help="Output path for --video.")
    parser.add_argument("--duration", type=float, default=50.0,
                        help="Video duration in seconds.")
    parser.add_argument("--fps", type=int, default=30,
                        help="Video frames per second.")
    parser.add_argument("--cycles", type=int, default=4,
                        help="Video pan segments.")
    parser.add_argument("--zoom", type=float, default=2.8,
                        help="Video maximum zoom.")
    parser.add_argument("--tour", choices=("cover", "classic"), default="cover",
                        help="Camera tour: cover (shows all photos) or classic.")
    parser.add_argument("--pan-speed", type=float, default=1.0, dest="pan_speed",
                        help="Camera speed between photos, 1.0 = normal.")
    parser.add_argument("--ffmpeg", default="ffmpeg",
                        help="ffmpeg binary or absolute path.")
    parser.add_argument("--audio-main", type=Path,
                        help="Main audio track path for --video.")
    parser.add_argument("--audio-bg", type=Path,
                        help="Background audio track path for --video.")
    parser.add_argument("--audio-main-vol", type=float, default=1.0,
                        help="Main track gain for --video.")
    parser.add_argument("--audio-bg-low", type=float, default=0.15,
                        help="Background gain while the main track plays.")
    parser.add_argument("--audio-bg-high", type=float, default=1.0,
                        help="Background gain after the main track ends.")
    parser.add_argument("--audio-fade", type=float, default=1.5,
                        help="Audio rise and ending fade length in seconds.")
    return parser.parse_args(argv)


def options_from_args(args: argparse.Namespace) -> CollageOptions:
    """Convert parsed command-line arguments into render options."""

    width = args.width
    if args.scale is not None:
        width = round(BASE_WIDTH * float(args.scale))
    if width is None:
        width = DEFAULT_WIDTH

    return CollageOptions(
        photos_dir=args.photos,
        out=args.out,
        cols=args.cols,
        width=width,
        radius=args.radius,
        gap=args.gap,
        background=parse_color(args.bg),
        shuffle=args.shuffle,
        seed=args.seed,
        video=args.video,
        video_out=args.video_out,
        video_duration=args.duration,
        video_fps=args.fps,
        video_cycles=args.cycles,
        video_zoom=args.zoom,
        video_tour=args.tour,
        video_pan_speed=args.pan_speed,
        ffmpeg=args.ffmpeg,
        audio_main=args.audio_main,
        audio_bg=args.audio_bg,
        audio_main_vol=args.audio_main_vol,
        audio_bg_low=args.audio_bg_low,
        audio_bg_high=args.audio_bg_high,
        audio_fade=args.audio_fade,
    )


def main(argv: list[str] | None = None) -> int:
    """Run the command-line collage renderer and return a process status code."""

    try:
        args = parse_args(argv)
        options = options_from_args(args)
        image_path = render_collage(options)
        render_optional_video(options, image_path)
    except CollageError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
