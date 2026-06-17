#!/usr/bin/env python3
"""Create a deterministic zoom-tour MP4 from a collage image.

This program takes a rendered collage, moves a vertical 9:16 camera over it with
a smooth zoom-and-pan path, and encodes the result as an MP4.

Author: Pasquale Marzaioli
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    from PIL import Image, ImageFilter, ImageOps
except ImportError as exc:
    raise SystemExit(
        "Pillow is required. Run `bash setup.sh` from this directory first."
    ) from exc


FORMAT_SIZES: dict[str, tuple[int, int]] = {
    "reel": (1080, 1920),
    "feed": (1080, 1350),
    "square": (1080, 1080),
}

# Sparse wander used by the "classic" tour.
ANCHORS: tuple[tuple[float, float], ...] = (
    (0.50, 0.50),
    (0.24, 0.25),
    (0.76, 0.34),
    (0.35, 0.73),
    (0.78, 0.78),
    (0.18, 0.55),
    (0.60, 0.18),
)

# Many stops spread across the whole collage, ordered as a gentle downward
# wander, used by the default "cover" tour so the camera passes over as many
# photos as possible. The last point returns toward the center for the closing
# pull-back.
COVER_ANCHORS: tuple[tuple[float, float], ...] = (
    (0.30, 0.14), (0.70, 0.12), (0.85, 0.24), (0.50, 0.28), (0.16, 0.26),
    (0.22, 0.42), (0.55, 0.40), (0.80, 0.46), (0.40, 0.54), (0.68, 0.60),
    (0.84, 0.66), (0.30, 0.64), (0.18, 0.74), (0.52, 0.72), (0.74, 0.80),
    (0.38, 0.84), (0.62, 0.88), (0.50, 0.50),
)


class VideoError(Exception):
    """Raised when video rendering cannot continue with the given inputs."""


@dataclass(frozen=True)
class CameraState:
    """Describe one frame camera mode, center, and crop zoom."""

    mode: str
    center_x: float
    center_y: float
    zoom: float


@dataclass(frozen=True)
class VideoOptions:
    """Store all user-facing settings needed to render a video."""

    image: Path
    out: Path
    format: str
    duration: float
    fps: int
    cycles: int
    zoom: float
    ffmpeg: str
    crf: int
    preset: str
    tour: str
    pan_speed: float


@dataclass(frozen=True)
class TourPlan:
    """Describe the active camera tour: which anchors to wander, how many hops
    to cross at pan speed 1.0, the maximum zoom, and the speed multiplier."""

    anchors: tuple[tuple[float, float], ...]
    base_segments: int
    max_zoom: float
    pan_speed: float


@dataclass
class FrameRenderer:
    """Cache source imagery and reusable wide frames for fast rendering."""

    source: Image.Image
    output_size: tuple[int, int]
    contain_frame: Image.Image


def clamp(value: float, minimum: float, maximum: float) -> float:
    """Clamp a float between inclusive minimum and maximum bounds."""

    return max(minimum, min(maximum, value))


def smoothstep(value: float) -> float:
    """Apply smoothstep easing to a normalized value."""

    x = clamp(value, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


def lerp(start: float, end: float, amount: float) -> float:
    """Interpolate between two floats using a normalized amount."""

    return start + (end - start) * amount


def lerp_point(
    start: tuple[float, float], end: tuple[float, float], amount: float
) -> tuple[float, float]:
    """Interpolate between two normalized two-dimensional points."""

    return (lerp(start[0], end[0], amount), lerp(start[1], end[1], amount))


def load_json_config(path: Path) -> dict[str, Any]:
    """Load a JSON config file and return its decoded object."""

    try:
        with path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
    except FileNotFoundError as exc:
        raise VideoError(f"Config file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise VideoError(f"Config file is not valid JSON: {path}") from exc
    if not isinstance(data, dict):
        raise VideoError(f"Config file must contain a JSON object: {path}")
    return data


def resolve_ffmpeg(binary: str) -> str:
    """Resolve an ffmpeg executable from a path or command name."""

    if Path(binary).is_file():
        return binary
    resolved = shutil.which(binary)
    if resolved is None:
        raise VideoError(
            "ffmpeg not found on PATH. Install it or pass `--ffmpeg <path>`."
        )
    return resolved


def make_contain_frame(source: Image.Image, output_size: tuple[int, int]) -> Image.Image:
    """Create a reusable wide frame that shows the full source image."""

    background = ImageOps.fit(source, output_size, Image.Resampling.LANCZOS)
    background = background.filter(ImageFilter.GaussianBlur(radius=28))
    overlay = Image.new("RGB", output_size, (255, 255, 255))
    background = Image.blend(background, overlay, 0.18)

    contained = source.copy()
    contained.thumbnail(output_size, Image.Resampling.LANCZOS)
    frame = background.copy()
    x = (output_size[0] - contained.width) // 2
    y = (output_size[1] - contained.height) // 2
    frame.paste(contained, (x, y))
    return frame


def create_frame_renderer(image_path: Path, output_size: tuple[int, int]) -> FrameRenderer:
    """Open the source image and prepare reusable state for frame rendering."""

    if not image_path.is_file():
        raise VideoError(f"Input image not found: {image_path}")
    try:
        with Image.open(image_path) as image:
            source = ImageOps.exif_transpose(image).convert("RGB")
    except OSError as exc:
        raise VideoError(f"Could not open input image: {image_path}") from exc

    if source.width < 2 or source.height < 2:
        raise VideoError("Input image is too small to render a video.")

    contain_frame = make_contain_frame(source, output_size)
    return FrameRenderer(
        source=source,
        output_size=output_size,
        contain_frame=contain_frame,
    )


def get_camera_state(progress: float, plan: TourPlan) -> CameraState:
    """Return the deterministic camera state for a normalized render progress.

    The path opens on the full collage, zooms in once, pans across the tour
    anchors for `base_segments` hops (scaled by `pan_speed`, looping over the
    anchors when faster), and pulls back out at the end.
    """

    open_hold = 0.075
    zoom_in_end = 0.20
    close_start = 0.92
    tour_zoom = max(1.05, plan.max_zoom)
    anchors = plan.anchors
    count = len(anchors)
    base = max(1, plan.base_segments)
    speed = plan.pan_speed if plan.pan_speed > 0.0 else 1.0

    if progress <= open_hold:
        return CameraState("contain", 0.5, 0.5, 1.0)

    if progress < zoom_in_end:
        amount = smoothstep((progress - open_hold) / (zoom_in_end - open_hold))
        center = lerp_point((0.5, 0.5), anchors[1], amount)
        zoom = lerp(1.05, tour_zoom, amount)
        return CameraState("crop", center[0], center[1], zoom)

    if progress >= close_start:
        amount = smoothstep((progress - close_start) / (1.0 - close_start))
        # Continue the pull-back from wherever the tour ended (no jump).
        last_segment = int(base * speed)
        start = anchors[(last_segment + 1) % count]
        center = lerp_point(start, (0.5, 0.5), amount)
        zoom = lerp(tour_zoom, 1.0, amount)
        if progress >= 0.999:
            return CameraState("contain", 0.5, 0.5, 1.0)
        return CameraState("crop", center[0], center[1], zoom)

    tour_progress = (progress - zoom_in_end) / (close_start - zoom_in_end)
    segment_position = tour_progress * base * speed
    segment_index = int(segment_position)
    segment_progress = smoothstep(segment_position - segment_index)
    start = anchors[(segment_index + 1) % count]
    end = anchors[(segment_index + 2) % count]
    center = lerp_point(start, end, segment_progress)
    return CameraState("crop", center[0], center[1], tour_zoom)


def crop_frame(renderer: FrameRenderer, state: CameraState) -> Image.Image:
    """Crop the source image with a 9:16 style viewport and resize it."""

    source = renderer.source
    output_width, output_height = renderer.output_size
    output_aspect = output_width / output_height
    source_aspect = source.width / source.height

    if source_aspect >= output_aspect:
        max_height = float(source.height)
        max_width = max_height * output_aspect
    else:
        max_width = float(source.width)
        max_height = max_width / output_aspect

    viewport_width = max_width / max(1.0, state.zoom)
    viewport_height = max_height / max(1.0, state.zoom)
    center_x = clamp(state.center_x * source.width,
                     viewport_width / 2, source.width - viewport_width / 2)
    center_y = clamp(state.center_y * source.height,
                     viewport_height / 2, source.height - viewport_height / 2)

    left = int(round(center_x - viewport_width / 2))
    top = int(round(center_y - viewport_height / 2))
    right = int(round(center_x + viewport_width / 2))
    bottom = int(round(center_y + viewport_height / 2))

    crop = source.crop((left, top, right, bottom))
    return crop.resize(renderer.output_size, Image.Resampling.LANCZOS)


def render_frame(renderer: FrameRenderer, state: CameraState) -> Image.Image:
    """Render a single RGB frame for a camera state."""

    if state.mode == "contain":
        return renderer.contain_frame.copy()
    return crop_frame(renderer, state)


def validate_options(options: VideoOptions) -> None:
    """Validate video options before invoking ffmpeg."""

    if options.format not in FORMAT_SIZES:
        valid = ", ".join(sorted(FORMAT_SIZES))
        raise VideoError(
            f"Unsupported format `{options.format}`. Use one of: {valid}.")
    if options.duration <= 0:
        raise VideoError("--duration must be greater than zero.")
    if options.fps <= 0 or options.fps > 120:
        raise VideoError("--fps must be between 1 and 120.")
    if options.cycles <= 0:
        raise VideoError("--cycles must be greater than zero.")
    if options.zoom < 1.0:
        raise VideoError("--zoom must be at least 1.0.")
    if options.crf < 0 or options.crf > 51:
        raise VideoError("--crf must be between 0 and 51.")
    if options.tour not in ("cover", "classic"):
        raise VideoError(
            f"Unsupported tour `{options.tour}`. Use one of: classic, cover.")
    if options.pan_speed <= 0:
        raise VideoError("--pan-speed must be greater than zero.")


def build_tour_plan(options: VideoOptions) -> TourPlan:
    """Build the camera tour plan from the options.

    The "cover" tour wanders all the spread-out anchors to show as many photos
    as possible; "classic" uses the sparse original path with `cycles` segments.
    """

    if options.tour == "classic":
        return TourPlan(ANCHORS, options.cycles, options.zoom, options.pan_speed)
    return TourPlan(
        COVER_ANCHORS, len(COVER_ANCHORS) - 1, options.zoom, options.pan_speed)


def render_video(options: VideoOptions) -> None:
    """Render an MP4 zoom-tour video with Pillow frames piped to ffmpeg."""

    validate_options(options)
    output_size = FORMAT_SIZES[options.format]
    renderer = create_frame_renderer(options.image, output_size)
    ffmpeg_path = resolve_ffmpeg(options.ffmpeg)
    frame_count = max(1, int(round(options.duration * options.fps)))
    tour_plan = build_tour_plan(options)
    options.out.parent.mkdir(parents=True, exist_ok=True)

    command = [
        ffmpeg_path,
        "-y",
        "-loglevel",
        "error",
        "-f",
        "rawvideo",
        "-vcodec",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-s",
        f"{output_size[0]}x{output_size[1]}",
        "-r",
        str(options.fps),
        "-i",
        "-",
        "-an",
        "-c:v",
        "libx264",
        "-preset",
        options.preset,
        "-crf",
        str(options.crf),
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        str(options.out),
    ]

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if process.stdin is None or process.stderr is None:
        raise VideoError("Could not open ffmpeg pipes.")

    try:
        for frame_index in range(frame_count):
            progress = frame_index / max(1, frame_count - 1)
            state = get_camera_state(progress, tour_plan)
            frame = render_frame(renderer, state)
            process.stdin.write(frame.convert("RGB").tobytes())
            if frame_index % max(1, options.fps * 5) == 0:
                print(
                    f"Rendered frame {frame_index + 1}/{frame_count}",
                    file=sys.stderr,
                )
    except BrokenPipeError as exc:
        stderr = process.stderr.read().decode("utf-8", errors="replace").strip()
        raise VideoError(
            f"ffmpeg stopped while receiving frames: {stderr}") from exc
    finally:
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass

    stderr = process.stderr.read().decode("utf-8", errors="replace").strip()
    return_code = process.wait()
    if return_code != 0:
        detail = f": {stderr}" if stderr else ""
        raise VideoError(f"ffmpeg failed with exit code {return_code}{detail}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    """Parse command-line arguments for video rendering."""

    parser = argparse.ArgumentParser(
        description="Render a 9:16 zoom-tour MP4.")
    parser.add_argument("--config", type=Path,
                        help="JSON config with video settings.")
    parser.add_argument("--image", type=Path, help="Source collage image.")
    parser.add_argument("--out", type=Path,
                        default=Path("tour.mp4"), help="Output MP4 path.")
    parser.add_argument(
        "--format", choices=sorted(FORMAT_SIZES), help="Output format.")
    parser.add_argument("--duration", type=float,
                        help="Video duration in seconds.")
    parser.add_argument("--fps", type=int, help="Frames per second.")
    parser.add_argument("--cycles", type=int,
                        help="Number of zoom and pan cycles.")
    parser.add_argument("--zoom", type=float, help="Maximum crop zoom.")
    parser.add_argument("--tour", choices=("cover", "classic"),
                        help="Camera tour: cover (shows all photos) or classic.")
    parser.add_argument("--pan-speed", type=float, dest="pan_speed",
                        help="Camera speed between photos, 1.0 = normal.")
    parser.add_argument("--ffmpeg", default=None,
                        help="ffmpeg binary or absolute path.")
    parser.add_argument("--crf", type=int,
                        help="H.264 quality, lower is better.")
    parser.add_argument("--preset", help="ffmpeg x264 preset.")
    return parser.parse_args(argv)


def options_from_args(args: argparse.Namespace) -> VideoOptions:
    """Merge CLI arguments and optional JSON config into video options."""

    config: dict[str, Any] = load_json_config(
        args.config) if args.config else {}
    video_config = config.get("video", {})
    if not isinstance(video_config, dict):
        raise VideoError("Config key `video` must be an object when present.")

    image_value = args.image or config.get(
        "image") or config.get("outputImage")
    if image_value is None:
        raise VideoError(
            "--image is required unless the config provides `image`.")

    return VideoOptions(
        image=Path(image_value),
        out=args.out,
        format=args.format or str(video_config.get("format", "reel")),
        duration=args.duration
        if args.duration is not None
        else float(video_config.get("duration", 50.0)),
        fps=args.fps if args.fps is not None else int(
            video_config.get("fps", 30)),
        cycles=args.cycles
        if args.cycles is not None
        else int(video_config.get("cycles", 4)),
        zoom=args.zoom
        if args.zoom is not None
        else float(video_config.get("zoom", video_config.get("maxZoom", 2.8))),
        ffmpeg=args.ffmpeg or str(video_config.get("ffmpeg", "ffmpeg")),
        crf=args.crf if args.crf is not None else int(
            video_config.get("crf", 18)),
        preset=args.preset or str(video_config.get("preset", "medium")),
        tour=args.tour or str(video_config.get("tour", "cover")),
        pan_speed=args.pan_speed
        if args.pan_speed is not None
        else float(video_config.get("panSpeed", video_config.get("pan_speed", 1.0))),
    )


def main(argv: list[str] | None = None) -> int:
    """Run the command-line video renderer and return a process status code."""

    try:
        args = parse_args(argv)
        options = options_from_args(args)
        render_video(options)
    except VideoError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
