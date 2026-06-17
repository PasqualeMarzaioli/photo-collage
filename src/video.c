/*
 * Create a deterministic zoom-tour MP4 from a collage image.
 *
 * This program takes a rendered collage, moves a vertical 9:16 camera over it
 * with a smooth zoom-and-pan path, and encodes the result as an MP4. It is a
 * thin command-line wrapper around the shared video renderer.
 *
 * Author: Pasquale Marzaioli
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
            "  --pan-speed F   Camera speed between photos, 1.0 = normal (default: 1.0)\n");
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
