/*
 * Zoom-tour video renderer.
 *
 * Given a collage image, this module moves a vertical 9:16 camera over it with
 * a deterministic zoom-and-pan path and pipes the rendered RGB frames to ffmpeg
 * for H.264 encoding. It is shared by both executables: the standalone `video`
 * tool and the `--video` option of the `collage` tool.
 *
 * Author: Pasquale Marzaioli
 *     Pasquale Marzaioli
 */

#ifndef VIDEO_RENDER_H
#define VIDEO_RENDER_H

#include <stdbool.h>

/* All user-facing settings needed to render a video. Strings are borrowed. */
typedef struct {
    const char *image;    /* source collage image path */
    const char *out;      /* output MP4 path */
    const char *format;   /* "reel", "feed", or "square" */
    double duration;      /* seconds */
    int fps;              /* frames per second */
    int cycles;           /* number of pan segments (classic tour only) */
    double zoom;          /* maximum crop zoom */
    const char *ffmpeg;   /* ffmpeg binary name or absolute path */
    int crf;              /* H.264 quality, lower is better */
    const char *preset;   /* x264 preset */
    const char *tour;     /* "cover" (default, shows all photos) or "classic" */
    double pan_speed;     /* camera speed multiplier between photos (default 1.0) */
    const char *audio_main; /* optional main audio track path */
    const char *audio_bg;   /* optional background audio track path */
    double audio_main_vol;  /* main track gain */
    double audio_bg_low;    /* background gain while main plays */
    double audio_bg_high;   /* background gain after main ends */
    double audio_fade;      /* rise and ending fade length in seconds */
} VideoOptions;

/*
 * Render the zoom-tour video described by options. Returns true on success and
 * false (after printing a clear message) on any validation, decoding, or
 * encoding failure.
 */
bool render_video(const VideoOptions *options);

#endif /* VIDEO_RENDER_H */
