/*
 * Create a deterministic zoom-tour MP4 from a collage image.
 *
 * This program takes a rendered collage, moves a vertical 9:16 camera over it
 * with a smooth zoom-and-pan path, and encodes the result as an MP4. It is a
 * thin command-line wrapper around the shared video renderer.
 *
 * Author: Pasquale Marzaioli
 *     Pasquale Marzaioli
 */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "video_render.h"

enum {
    OPT_IMAGE = 1000,
    OPT_OUT,
    OPT_FORMAT,
    OPT_DURATION,
    OPT_FPS,
    OPT_CYCLES,
    OPT_ZOOM,
    OPT_FFMPEG,
    OPT_CRF,
    OPT_PRESET,
    OPT_TOUR,
    OPT_PAN_SPEED,
    OPT_AUDIO_MAIN,
    OPT_AUDIO_BG,
    OPT_AUDIO_MAIN_VOL,
    OPT_AUDIO_BG_LOW,
    OPT_AUDIO_BG_HIGH,
    OPT_AUDIO_FADE,
    OPT_HELP,
};

static void print_usage(void)
{
    fprintf(stderr,
            "Render a 9:16 zoom-tour MP4.\n"
            "\n"
            "Usage: video --image PATH [options]\n"
            "  --image PATH    Source collage image (required)\n"
            "  --out PATH      Output MP4 path (default: tour.mp4)\n"
            "  --format NAME   Output format: reel, feed, or square (default: reel)\n"
            "  --duration S    Video duration in seconds (default: 50)\n"
            "  --fps N         Frames per second (default: 30)\n"
            "  --cycles N      Number of zoom and pan cycles (default: 4)\n"
            "  --zoom F        Maximum crop zoom (default: 2.8)\n"
            "  --ffmpeg PATH   ffmpeg binary or absolute path\n"
            "  --crf N         H.264 quality, lower is better (default: 18)\n"
            "  --preset NAME   ffmpeg x264 preset (default: medium)\n"
            "  --tour NAME     Camera tour: cover (shows all photos) or classic\n"
            "                  (default: cover)\n"
            "  --pan-speed F   Camera speed between photos, 1.0 = normal (default: 1.0)\n"
            "  --audio-main PATH      Main audio track path\n"
            "  --audio-bg PATH        Background audio track path\n"
            "  --audio-main-vol F     Main track gain (default: 1.0)\n"
            "  --audio-bg-low F       Background gain while main plays (default: 0.15)\n"
            "  --audio-bg-high F      Background gain after main ends (default: 1.0)\n"
            "  --audio-fade F         Audio rise and ending fade seconds (default: 1.5)\n");
}

static bool parse_int(const char *name, const char *text, long *out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0) {
        fprintf(stderr, "Invalid integer for %s: %s\n", name, text);
        return false;
    }
    *out = value;
    return true;
}

static bool parse_double(const char *name, const char *text, double *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || *end != '\0' || errno != 0) {
        fprintf(stderr, "Invalid number for %s: %s\n", name, text);
        return false;
    }
    *out = value;
    return true;
}

int main(int argc, char **argv)
{
    VideoOptions options = {
        .image = NULL,
        .out = "tour.mp4",
        .format = "reel",
        .duration = 50.0,
        .fps = 30,
        .cycles = 4,
        .zoom = 2.8,
        .ffmpeg = "ffmpeg",
        .crf = 18,
        .preset = "medium",
        .tour = "cover",
        .pan_speed = 1.0,
        .audio_main = NULL,
        .audio_bg = NULL,
        .audio_main_vol = 1.0,
        .audio_bg_low = 0.15,
        .audio_bg_high = 1.0,
        .audio_fade = 1.5,
    };

    static struct option long_opts[] = {
        {"image", required_argument, 0, OPT_IMAGE},
        {"out", required_argument, 0, OPT_OUT},
        {"format", required_argument, 0, OPT_FORMAT},
        {"duration", required_argument, 0, OPT_DURATION},
        {"fps", required_argument, 0, OPT_FPS},
        {"cycles", required_argument, 0, OPT_CYCLES},
        {"zoom", required_argument, 0, OPT_ZOOM},
        {"ffmpeg", required_argument, 0, OPT_FFMPEG},
        {"crf", required_argument, 0, OPT_CRF},
        {"preset", required_argument, 0, OPT_PRESET},
        {"tour", required_argument, 0, OPT_TOUR},
        {"pan-speed", required_argument, 0, OPT_PAN_SPEED},
        {"audio-main", required_argument, 0, OPT_AUDIO_MAIN},
        {"audio-bg", required_argument, 0, OPT_AUDIO_BG},
        {"audio-main-vol", required_argument, 0, OPT_AUDIO_MAIN_VOL},
        {"audio-bg-low", required_argument, 0, OPT_AUDIO_BG_LOW},
        {"audio-bg-high", required_argument, 0, OPT_AUDIO_BG_HIGH},
        {"audio-fade", required_argument, 0, OPT_AUDIO_FADE},
        {"help", no_argument, 0, OPT_HELP},
        {0, 0, 0, 0},
    };

    int c;
    long iv;
    double dv;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case OPT_IMAGE: options.image = optarg; break;
        case OPT_OUT: options.out = optarg; break;
        case OPT_FORMAT: options.format = optarg; break;
        case OPT_DURATION:
            if (!parse_double("--duration", optarg, &dv)) return 2;
            options.duration = dv;
            break;
        case OPT_FPS:
            if (!parse_int("--fps", optarg, &iv)) return 2;
            options.fps = (int)iv;
            break;
        case OPT_CYCLES:
            if (!parse_int("--cycles", optarg, &iv)) return 2;
            options.cycles = (int)iv;
            break;
        case OPT_ZOOM:
            if (!parse_double("--zoom", optarg, &dv)) return 2;
            options.zoom = dv;
            break;
        case OPT_FFMPEG: options.ffmpeg = optarg; break;
        case OPT_CRF:
            if (!parse_int("--crf", optarg, &iv)) return 2;
            options.crf = (int)iv;
            break;
        case OPT_PRESET: options.preset = optarg; break;
        case OPT_TOUR: options.tour = optarg; break;
        case OPT_PAN_SPEED:
            if (!parse_double("--pan-speed", optarg, &dv)) return 2;
            options.pan_speed = dv;
            break;
        case OPT_AUDIO_MAIN: options.audio_main = optarg; break;
        case OPT_AUDIO_BG: options.audio_bg = optarg; break;
        case OPT_AUDIO_MAIN_VOL:
            if (!parse_double("--audio-main-vol", optarg, &dv)) return 2;
            options.audio_main_vol = dv;
            break;
        case OPT_AUDIO_BG_LOW:
            if (!parse_double("--audio-bg-low", optarg, &dv)) return 2;
            options.audio_bg_low = dv;
            break;
        case OPT_AUDIO_BG_HIGH:
            if (!parse_double("--audio-bg-high", optarg, &dv)) return 2;
            options.audio_bg_high = dv;
            break;
        case OPT_AUDIO_FADE:
            if (!parse_double("--audio-fade", optarg, &dv)) return 2;
            options.audio_fade = dv;
            break;
        case OPT_HELP: print_usage(); return 0;
        default: return 2;
        }
    }

    if (options.image == NULL) {
        fprintf(stderr, "--image is required.\n");
        return 1;
    }

    return render_video(&options) ? 0 : 1;
}
