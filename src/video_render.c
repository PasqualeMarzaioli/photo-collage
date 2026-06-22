/*
 * Zoom-tour video renderer implementation.
 *
 * The camera path is deterministic: the frame opens on the whole collage,
 * zooms in once, pans through a fixed set of anchor points at a steady zoom,
 * and pulls back out at the end. Frames are rendered in parallel batches and
 * written to ffmpeg in order through a pipe, so encoding overlaps rendering
 * while the output stream stays sequential.
 *
 * Author: Pasquale Marzaioli
 */

#define _GNU_SOURCE

#include "video_render.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "image.h"
#include "parallel.h"

/* Output frame sizes for each supported format. */
typedef struct
{
    const char *name;
    int width;
    int height;
} FormatSize;

static const FormatSize kFormats[] = {
    {"reel", 1080, 1920},
    {"feed", 1080, 1350},
    {"square", 1080, 1080},
};
static const int kFormatCount = (int)(sizeof(kFormats) / sizeof(kFormats[0]));

/* Normalised anchor points for the "classic" tour: a sparse wander. */
static const double kAnchors[][2] = {
    {0.50, 0.50},
    {0.24, 0.25},
    {0.76, 0.34},
    {0.35, 0.73},
    {0.78, 0.78},
    {0.18, 0.55},
    {0.60, 0.18},
};
static const int kAnchorCount = (int)(sizeof(kAnchors) / sizeof(kAnchors[0]));

/*
 * Normalised anchor points for the "cover" tour: many stops spread across the
 * whole collage, ordered as a gentle downward wander, so over the video the
 * camera passes over as many photos as possible. The last point returns toward
 * the centre for a clean closing pull-back.
 */
static const double kCoverAnchors[][2] = {
    {0.30, 0.14},
    {0.70, 0.12},
    {0.85, 0.24},
    {0.50, 0.28},
    {0.16, 0.26},
    {0.22, 0.42},
    {0.55, 0.40},
    {0.80, 0.46},
    {0.40, 0.54},
    {0.68, 0.60},
    {0.84, 0.66},
    {0.30, 0.64},
    {0.18, 0.74},
    {0.52, 0.72},
    {0.74, 0.80},
    {0.38, 0.84},
    {0.62, 0.88},
    {0.50, 0.50},
};
static const int kCoverAnchorCount = (int)(sizeof(kCoverAnchors) / sizeof(kCoverAnchors[0]));

/* One frame's camera mode, normalised centre, and crop zoom. */
typedef struct
{
    int is_contain; /* non-zero shows the full collage instead of a crop */
    double cx;
    double cy;
    double zoom;
} CameraState;

/* The active tour: which anchor set to wander, how many segments to cross, and
   how fast. base_segments is the number of anchor-to-anchor hops spread across
   the tour at pan_speed 1.0 (the full cover set, or `cycles` for classic). */
typedef struct
{
    const double (*anchors)[2];
    int anchor_count;
    int base_segments;
    double max_zoom;
    double pan_speed;
} TourPlan;

/* Cached source imagery and the reusable wide "contain" frame. */
typedef struct
{
    Image *source;
    int out_w;
    int out_h;
    Image *contain_frame;
} FrameRenderer;

/* Growable, NULL-terminated argv storage for ffmpeg invocations. */
typedef struct
{
    char **items;
    size_t count;
    size_t capacity;
} ArgvBuilder;

/* --- small maths helpers ------------------------------------------------ */

static double clampd(double value, double lo, double hi)
{
    if (value < lo)
    {
        return lo;
    }
    if (value > hi)
    {
        return hi;
    }
    return value;
}

static double smoothstep(double value)
{
    double x = clampd(value, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

static double lerp(double start, double end, double amount)
{
    return start + (end - start) * amount;
}

static CameraState make_state(int contain, double cx, double cy, double zoom)
{
    CameraState state;
    state.is_contain = contain;
    state.cx = cx;
    state.cy = cy;
    state.zoom = zoom;
    return state;
}

/* --- camera path -------------------------------------------------------- */

/*
 * Return the camera state for a normalised render progress in [0, 1]: an
 * opening hold on the full collage, a single zoom-in, a steady segmented pan
 * across the tour anchors, and a final pull-back to the full collage. The pan
 * traverses `base_segments` hops at pan_speed 1.0; a higher pan_speed moves the
 * camera faster (and loops over the anchors), showing more photos in the time.
 */
static CameraState get_camera_state(double progress, const TourPlan *plan)
{
    const double open_hold = 0.075;
    const double zoom_in_end = 0.20;
    const double close_start = 0.92;
    double tour_zoom = plan->max_zoom > 1.05 ? plan->max_zoom : 1.05;
    const double (*anchors)[2] = plan->anchors;
    int n = plan->anchor_count;
    int base = plan->base_segments > 1 ? plan->base_segments : 1;
    double speed = plan->pan_speed > 0.0 ? plan->pan_speed : 1.0;

    if (progress <= open_hold)
    {
        return make_state(1, 0.5, 0.5, 1.0);
    }

    if (progress < zoom_in_end)
    {
        double amount = smoothstep((progress - open_hold) / (zoom_in_end - open_hold));
        double cx = lerp(0.5, anchors[1][0], amount);
        double cy = lerp(0.5, anchors[1][1], amount);
        double zoom = lerp(1.05, tour_zoom, amount);
        return make_state(0, cx, cy, zoom);
    }

    if (progress >= close_start)
    {
        double amount = smoothstep((progress - close_start) / (1.0 - close_start));
        /* Continue the pull-back from wherever the tour ended (no jump). */
        int last_seg = (int)(base * speed);
        int si = (last_seg + 1) % n;
        double cx = lerp(anchors[si][0], 0.5, amount);
        double cy = lerp(anchors[si][1], 0.5, amount);
        double zoom = lerp(tour_zoom, 1.0, amount);
        if (progress >= 0.999)
        {
            return make_state(1, 0.5, 0.5, 1.0);
        }
        return make_state(0, cx, cy, zoom);
    }

    double tour_progress = (progress - zoom_in_end) / (close_start - zoom_in_end);
    double segment_pos = tour_progress * base * speed;
    int segment_index = (int)segment_pos;
    double segment_progress = smoothstep(segment_pos - segment_index);
    int i0 = (segment_index + 1) % n;
    int i1 = (segment_index + 2) % n;
    double cx = lerp(anchors[i0][0], anchors[i1][0], segment_progress);
    double cy = lerp(anchors[i0][1], anchors[i1][1], segment_progress);
    return make_state(0, cx, cy, tour_zoom);
}

/* --- frame rendering ---------------------------------------------------- */

/*
 * Build the reusable wide frame: the full collage cover-filled and blurred as a
 * soft background, lightened toward white, with the whole collage shown
 * contained and centred on top.
 */
static Image *make_contain_frame(const Image *source, int out_w, int out_h)
{
    Image *background = image_fit_cover(source, out_w, out_h);
    if (background == NULL)
    {
        return NULL;
    }
    gaussian_blur(background, 28.0);

    Image *white = image_new(out_w, out_h, 3);
    if (white == NULL)
    {
        image_free(background);
        return NULL;
    }
    image_fill(white, 255, 255, 255);
    image_blend(background, white, 0.18);
    image_free(white);

    Image *contained = image_fit_contain(source, out_w, out_h);
    if (contained == NULL)
    {
        image_free(background);
        return NULL;
    }
    int x = (out_w - contained->width) / 2;
    int y = (out_h - contained->height) / 2;
    paste_with_alpha(background, contained, NULL, x, y);
    image_free(contained);
    return background;
}

/* Open the source image and prepare reusable state. Returns false on failure. */
static bool create_frame_renderer(const char *path, int out_w, int out_h,
                                  FrameRenderer *renderer)
{
    if (access(path, F_OK) != 0)
    {
        fprintf(stderr, "Input image not found: %s\n", path);
        return false;
    }
    Image *source = image_load(path);
    if (source == NULL)
    {
        return false; /* image_load already reported the reason */
    }
    if (source->width < 2 || source->height < 2)
    {
        fprintf(stderr, "Input image is too small to render a video.\n");
        image_free(source);
        return false;
    }
    Image *contain_frame = make_contain_frame(source, out_w, out_h);
    if (contain_frame == NULL)
    {
        fprintf(stderr, "Out of memory preparing the video background.\n");
        image_free(source);
        return false;
    }
    renderer->source = source;
    renderer->out_w = out_w;
    renderer->out_h = out_h;
    renderer->contain_frame = contain_frame;
    return true;
}

static void free_frame_renderer(FrameRenderer *renderer)
{
    image_free(renderer->source);
    image_free(renderer->contain_frame);
    renderer->source = NULL;
    renderer->contain_frame = NULL;
}

/* Crop a 9:16 viewport at the given zoom and centre, then resize to output. */
static Image *crop_frame(const FrameRenderer *renderer, const CameraState *state)
{
    const Image *src = renderer->source;
    double out_aspect = (double)renderer->out_w / (double)renderer->out_h;
    double src_aspect = (double)src->width / (double)src->height;

    double max_w;
    double max_h;
    if (src_aspect >= out_aspect)
    {
        max_h = src->height;
        max_w = max_h * out_aspect;
    }
    else
    {
        max_w = src->width;
        max_h = max_w / out_aspect;
    }

    double zoom = state->zoom < 1.0 ? 1.0 : state->zoom;
    double viewport_w = max_w / zoom;
    double viewport_h = max_h / zoom;
    double cx = clampd(state->cx * src->width, viewport_w / 2.0,
                       src->width - viewport_w / 2.0);
    double cy = clampd(state->cy * src->height, viewport_h / 2.0,
                       src->height - viewport_h / 2.0);

    double left = py_round(cx - viewport_w / 2.0);
    double top = py_round(cy - viewport_h / 2.0);
    double right = py_round(cx + viewport_w / 2.0);
    double bottom = py_round(cy + viewport_h / 2.0);
    if (right < left + 1)
    {
        right = left + 1;
    }
    if (bottom < top + 1)
    {
        bottom = top + 1;
    }
    return resample_region(src, left, top, right - left, bottom - top,
                           renderer->out_w, renderer->out_h);
}

/* Render a single frame for a camera state. Returns NULL on allocation failure. */
static Image *render_frame(const FrameRenderer *renderer, const CameraState *state)
{
    if (state->is_contain)
    {
        return image_copy(renderer->contain_frame);
    }
    return crop_frame(renderer, state);
}

/* --- ffmpeg plumbing ---------------------------------------------------- */

/* Resolve an ffmpeg executable from a path or a PATH lookup; caller frees. */
static char *resolve_ffmpeg(const char *binary)
{
    if (strchr(binary, '/') != NULL)
    {
        if (access(binary, X_OK) == 0)
        {
            return strdup(binary);
        }
        fprintf(stderr,
                "ffmpeg not found on PATH. Install it or pass `--ffmpeg <path>`.\n");
        return NULL;
    }
    const char *path = getenv("PATH");
    if (path != NULL)
    {
        char *copy = strdup(path);
        if (copy == NULL)
        {
            return NULL;
        }
        char *save = NULL;
        for (char *dir = strtok_r(copy, ":", &save); dir != NULL;
             dir = strtok_r(NULL, ":", &save))
        {
            char full[4096];
            snprintf(full, sizeof full, "%s/%s", dir, binary);
            if (access(full, X_OK) == 0)
            {
                char *resolved = strdup(full);
                free(copy);
                return resolved;
            }
        }
        free(copy);
    }
    fprintf(stderr,
            "ffmpeg not found on PATH. Install it or pass `--ffmpeg <path>`.\n");
    return NULL;
}

/*
 * Resolve ffprobe from the selected ffmpeg path. When ffmpeg ends in the usual
 * executable name, the sibling ffprobe is preferred; otherwise PATH is used.
 * Returns a malloc'd path, or NULL after printing a clear error.
 */
static char *resolve_ffprobe(const char *ffmpeg)
{
    const char *slash = strrchr(ffmpeg, '/');
    const char *name = slash != NULL ? slash + 1 : ffmpeg;
    const char *replacement = NULL;
    if (strcmp(name, "ffmpeg") == 0)
    {
        replacement = "ffprobe";
    }
    else if (strcmp(name, "ffmpeg.exe") == 0)
    {
        replacement = "ffprobe.exe";
    }

    if (replacement != NULL)
    {
        char candidate[4096];
        if (slash != NULL)
        {
            size_t prefix_len = (size_t)(slash - ffmpeg + 1);
            if (prefix_len + strlen(replacement) < sizeof candidate)
            {
                memcpy(candidate, ffmpeg, prefix_len);
                strcpy(candidate + prefix_len, replacement);
                if (access(candidate, X_OK) == 0)
                {
                    return strdup(candidate);
                }
            }
        }
        else if (access(replacement, X_OK) == 0)
        {
            return strdup(replacement);
        }
    }

    const char *path = getenv("PATH");
    if (path != NULL)
    {
        char *copy = strdup(path);
        if (copy == NULL)
        {
            return NULL;
        }
        char *save = NULL;
        for (char *dir = strtok_r(copy, ":", &save); dir != NULL;
             dir = strtok_r(NULL, ":", &save))
        {
            char full[4096];
            snprintf(full, sizeof full, "%s/ffprobe", dir);
            if (access(full, X_OK) == 0)
            {
                char *resolved = strdup(full);
                free(copy);
                return resolved;
            }
        }
        free(copy);
    }

    fprintf(stderr,
            "ffprobe not found on PATH. Install ffmpeg/ffprobe or pass "
            "`--ffmpeg <path>` next to ffprobe.\n");
    return NULL;
}

/*
 * Probe an audio file duration with ffprobe. ffmpeg is the selected executable
 * path used to derive ffprobe, path is the audio file, and out_duration receives
 * seconds on success. Returns true on success and false after printing an error.
 */
static bool probe_duration(const char *ffmpeg, const char *path, double *out_duration)
{
    char *ffprobe = resolve_ffprobe(ffmpeg);
    if (ffprobe == NULL)
    {
        return false;
    }

    int fds[2];
    if (pipe(fds) != 0)
    {
        fprintf(stderr, "Could not create ffprobe pipe.\n");
        free(ffprobe);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Could not start ffprobe.\n");
        close(fds[0]);
        close(fds[1]);
        free(ffprobe);
        return false;
    }
    if (pid == 0)
    {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execlp(ffprobe, ffprobe,
               "-v", "error",
               "-show_entries", "format=duration",
               "-of", "default=noprint_wrappers=1:nokey=1",
               path,
               (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    char buffer[256];
    size_t used = 0;
    for (;;)
    {
        char chunk[128];
        ssize_t n = read(fds[0], chunk, sizeof chunk);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            fprintf(stderr, "Could not read ffprobe output.\n");
            close(fds[0]);
            free(ffprobe);
            return false;
        }
        if (n == 0)
        {
            break;
        }
        size_t available = sizeof buffer - 1 - used;
        size_t copy = (size_t)n < available ? (size_t)n : available;
        if (copy > 0)
        {
            memcpy(buffer + used, chunk, copy);
            used += copy;
        }
    }
    close(fds[0]);
    buffer[used] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    free(ffprobe);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        fprintf(stderr, "Could not probe audio duration for %s.\n", path);
        return false;
    }

    char *end = NULL;
    errno = 0;
    double value = strtod(buffer, &end);
    if (end == buffer || errno != 0 || !isfinite(value) || value < 0.0)
    {
        fprintf(stderr, "ffprobe did not return a valid duration for audio file: %s\n",
                path);
        return false;
    }
    *out_duration = value;
    return true;
}

/*
 * Initialise a growable argv builder. capacity is the initial number of usable
 * argument slots, excluding the maintained NULL terminator. Returns true on
 * success and false on allocation failure.
 */
static bool argv_init(ArgvBuilder *argv, size_t capacity)
{
    argv->count = 0;
    argv->capacity = capacity > 0 ? capacity : 16;
    argv->items = (char **)calloc(argv->capacity + 1, sizeof(char *));
    return argv->items != NULL;
}

/*
 * Append one borrowed argument pointer to a growable argv. The vector remains
 * NULL-terminated after every successful push. Returns true on success.
 */
static bool argv_push(ArgvBuilder *argv, char *item)
{
    if (argv->count == argv->capacity)
    {
        size_t next = argv->capacity * 2;
        char **grown = (char **)realloc(argv->items, (next + 1) * sizeof(char *));
        if (grown == NULL)
        {
            return false;
        }
        argv->items = grown;
        argv->capacity = next;
    }
    argv->items[argv->count++] = item;
    argv->items[argv->count] = NULL;
    return true;
}

/*
 * Release argv storage. Argument strings are borrowed by design, so only the
 * vector itself is freed.
 */
static void argv_free(ArgvBuilder *argv)
{
    free(argv->items);
    argv->items = NULL;
    argv->count = 0;
    argv->capacity = 0;
}

/*
 * Build the ffmpeg audio filtergraph for the requested optional audio inputs.
 * Input indexes are ffmpeg stream indexes after the raw video pipe. The buffer
 * receives a complete filter_complex value. Returns true on success.
 */
static bool build_audio_filter(const VideoOptions *options, const char *ffmpeg,
                               int bg_index, int main_index, double video_dur,
                               char *buffer, size_t buffer_size)
{
    double main_end = 0.0;
    if (main_index >= 0 &&
        !probe_duration(ffmpeg, options->audio_main, &main_end))
    {
        return false;
    }

    double fade = options->audio_fade;
    double fade_start = video_dur - fade;
    if (fade_start < 0.0)
    {
        fade_start = 0.0;
    }

    int written = 0;
    if (main_index >= 0 && bg_index >= 0)
    {
        if (fade > 0.0)
        {
            written = snprintf(
                buffer, buffer_size,
                "[%d:a]volume=%.6f[am];"
                "[%d:a]volume='%.6f+(%.6f-%.6f)*clip((t-%.6f)/%.6f\\,0\\,1)':eval=frame[bg];"
                "[am][bg]amix=inputs=2:duration=longest:normalize=0[mix];"
                "[mix]afade=t=out:st=%.6f:d=%.6f[aout]",
                main_index, options->audio_main_vol,
                bg_index, options->audio_bg_low, options->audio_bg_high,
                options->audio_bg_low, main_end, fade,
                fade_start, fade);
        }
        else
        {
            written = snprintf(
                buffer, buffer_size,
                "[%d:a]volume=%.6f[am];"
                "[%d:a]volume='%.6f+(%.6f-%.6f)*gte(t\\,%.6f)':eval=frame[bg];"
                "[am][bg]amix=inputs=2:duration=longest:normalize=0[mix];"
                "[mix]anull[aout]",
                main_index, options->audio_main_vol,
                bg_index, options->audio_bg_low, options->audio_bg_high,
                options->audio_bg_low, main_end);
        }
    }
    else if (main_index >= 0)
    {
        if (fade > 0.0)
        {
            written = snprintf(
                buffer, buffer_size,
                "[%d:a]volume=%.6f[am];"
                "[am]afade=t=out:st=%.6f:d=%.6f[aout]",
                main_index, options->audio_main_vol, fade_start, fade);
        }
        else
        {
            written = snprintf(
                buffer, buffer_size,
                "[%d:a]volume=%.6f[am];[am]anull[aout]",
                main_index, options->audio_main_vol);
        }
    }
    else if (bg_index >= 0)
    {
        if (fade > 0.0)
        {
            written = snprintf(
                buffer, buffer_size,
                "[%d:a]volume=%.6f[bg];"
                "[bg]afade=t=out:st=%.6f:d=%.6f[aout]",
                bg_index, options->audio_bg_high, fade_start, fade);
        }
        else
        {
            written = snprintf(
                buffer, buffer_size,
                "[%d:a]volume=%.6f[bg];[bg]anull[aout]",
                bg_index, options->audio_bg_high);
        }
    }
    else
    {
        fprintf(stderr, "No audio inputs were provided for the audio filter.\n");
        return false;
    }

    if (written < 0 || (size_t)written >= buffer_size)
    {
        fprintf(stderr, "Audio filter is too long.\n");
        return false;
    }
    return true;
}

/* Start ffmpeg with its stdin connected to a pipe we write frames into. */
static pid_t spawn_ffmpeg(char *const argv[], int *write_fd)
{
    int fds[2];
    if (pipe(fds) != 0)
    {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0)
    {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[0]);
    *write_fd = fds[1];
    return pid;
}

/* Write the whole buffer, retrying short writes; false on a broken pipe. */
static bool write_all(int fd, const uint8_t *buf, size_t n)
{
    size_t offset = 0;
    while (offset < n)
    {
        ssize_t written = write(fd, buf + offset, n - offset);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

/* --- validation --------------------------------------------------------- */

/*
 * Return whether the options request any audio stream. Empty strings are treated
 * as absent paths because argv callers use NULL for omitted values.
 */
static bool has_audio(const VideoOptions *o)
{
    return (o->audio_main != NULL && o->audio_main[0] != '\0') ||
           (o->audio_bg != NULL && o->audio_bg[0] != '\0');
}

static const FormatSize *find_format(const char *name)
{
    for (int i = 0; i < kFormatCount; ++i)
    {
        if (strcmp(kFormats[i].name, name) == 0)
        {
            return &kFormats[i];
        }
    }
    return NULL;
}

static bool validate_options(const VideoOptions *o)
{
    if (find_format(o->format) == NULL)
    {
        fprintf(stderr,
                "Unsupported format `%s`. Use one of: feed, reel, square.\n",
                o->format);
        return false;
    }
    if (o->duration <= 0.0)
    {
        fprintf(stderr, "--duration must be greater than zero.\n");
        return false;
    }
    if (o->fps <= 0 || o->fps > 120)
    {
        fprintf(stderr, "--fps must be between 1 and 120.\n");
        return false;
    }
    if (o->cycles <= 0)
    {
        fprintf(stderr, "--cycles must be greater than zero.\n");
        return false;
    }
    if (o->zoom < 1.0)
    {
        fprintf(stderr, "--zoom must be at least 1.0.\n");
        return false;
    }
    if (o->crf < 0 || o->crf > 51)
    {
        fprintf(stderr, "--crf must be between 0 and 51.\n");
        return false;
    }
    if (strcmp(o->tour, "cover") != 0 && strcmp(o->tour, "classic") != 0)
    {
        fprintf(stderr, "Unsupported tour `%s`. Use one of: classic, cover.\n",
                o->tour);
        return false;
    }
    if (o->pan_speed <= 0.0)
    {
        fprintf(stderr, "--pan-speed must be greater than zero.\n");
        return false;
    }
    if (o->audio_main != NULL && o->audio_main[0] != '\0' &&
        access(o->audio_main, F_OK) != 0)
    {
        fprintf(stderr, "Audio main track not found: %s\n", o->audio_main);
        return false;
    }
    if (o->audio_bg != NULL && o->audio_bg[0] != '\0' &&
        access(o->audio_bg, F_OK) != 0)
    {
        fprintf(stderr, "Audio background track not found: %s\n", o->audio_bg);
        return false;
    }
    if (!isfinite(o->audio_main_vol) || o->audio_main_vol < 0.0)
    {
        fprintf(stderr, "--audio-main-vol must be greater than or equal to zero.\n");
        return false;
    }
    if (!isfinite(o->audio_bg_low) || o->audio_bg_low < 0.0)
    {
        fprintf(stderr, "--audio-bg-low must be greater than or equal to zero.\n");
        return false;
    }
    if (!isfinite(o->audio_bg_high) || o->audio_bg_high < 0.0)
    {
        fprintf(stderr, "--audio-bg-high must be greater than or equal to zero.\n");
        return false;
    }
    if (!isfinite(o->audio_fade) || o->audio_fade < 0.0)
    {
        fprintf(stderr, "--audio-fade must be greater than or equal to zero.\n");
        return false;
    }
    return true;
}

/* --- parallel batch rendering ------------------------------------------- */

/* Shared state for rendering one batch of consecutive frames in parallel. */
typedef struct
{
    const FrameRenderer *renderer;
    const TourPlan *plan;
    int frame_count;
    int start;
    Image **frames;
} BatchContext;

static void render_batch_one(void *context, int k)
{
    BatchContext *ctx = (BatchContext *)context;
    int index = ctx->start + k;
    int denominator = ctx->frame_count - 1;
    if (denominator < 1)
    {
        denominator = 1;
    }
    double progress = (double)index / (double)denominator;
    CameraState state = get_camera_state(progress, ctx->plan);
    ctx->frames[k] = render_frame(ctx->renderer, &state);
}

bool render_video(const VideoOptions *options)
{
    if (!validate_options(options))
    {
        return false;
    }
    const FormatSize *format = find_format(options->format);
    int out_w = format->width;
    int out_h = format->height;

    FrameRenderer renderer;
    if (!create_frame_renderer(options->image, out_w, out_h, &renderer))
    {
        return false;
    }

    char *ffmpeg_path = resolve_ffmpeg(options->ffmpeg);
    if (ffmpeg_path == NULL)
    {
        free_frame_renderer(&renderer);
        return false;
    }

    int frame_count = (int)py_round(options->duration * options->fps);
    if (frame_count < 1)
    {
        frame_count = 1;
    }

    /* Pick the tour: "cover" wanders all the spread-out anchors to show as many
       photos as possible; "classic" uses the sparse original path. */
    int cover = strcmp(options->tour, "classic") != 0;
    TourPlan plan;
    plan.anchors = cover ? kCoverAnchors : kAnchors;
    plan.anchor_count = cover ? kCoverAnchorCount : kAnchorCount;
    plan.base_segments = cover ? (kCoverAnchorCount - 1) : options->cycles;
    plan.max_zoom = options->zoom;
    plan.pan_speed = options->pan_speed;

    char size_arg[32];
    char fps_arg[16];
    char crf_arg[16];
    char duration_arg[64];
    char audio_filter[4096];
    snprintf(size_arg, sizeof size_arg, "%dx%d", out_w, out_h);
    snprintf(fps_arg, sizeof fps_arg, "%d", options->fps);
    snprintf(crf_arg, sizeof crf_arg, "%d", options->crf);
    double video_dur = (double)frame_count / (double)options->fps;
    snprintf(duration_arg, sizeof duration_arg, "%.6f", video_dur);

    ArgvBuilder argv;
    if (!argv_init(&argv, 32))
    {
        fprintf(stderr, "Out of memory building the ffmpeg command.\n");
        free(ffmpeg_path);
        free_frame_renderer(&renderer);
        return false;
    }

    bool argv_ok = true;
    argv_ok = argv_ok && argv_push(&argv, ffmpeg_path);
    argv_ok = argv_ok && argv_push(&argv, "-y");
    argv_ok = argv_ok && argv_push(&argv, "-loglevel");
    argv_ok = argv_ok && argv_push(&argv, "error");
    argv_ok = argv_ok && argv_push(&argv, "-f");
    argv_ok = argv_ok && argv_push(&argv, "rawvideo");
    argv_ok = argv_ok && argv_push(&argv, "-vcodec");
    argv_ok = argv_ok && argv_push(&argv, "rawvideo");
    argv_ok = argv_ok && argv_push(&argv, "-pix_fmt");
    argv_ok = argv_ok && argv_push(&argv, "rgb24");
    argv_ok = argv_ok && argv_push(&argv, "-s");
    argv_ok = argv_ok && argv_push(&argv, size_arg);
    argv_ok = argv_ok && argv_push(&argv, "-r");
    argv_ok = argv_ok && argv_push(&argv, fps_arg);
    argv_ok = argv_ok && argv_push(&argv, "-i");
    argv_ok = argv_ok && argv_push(&argv, "-");

    int bg_index = -1;
    int main_index = -1;
    int next_input = 1;
    if (has_audio(options))
    {
        if (options->audio_bg != NULL && options->audio_bg[0] != '\0')
        {
            bg_index = next_input++;
            argv_ok = argv_ok && argv_push(&argv, "-stream_loop");
            argv_ok = argv_ok && argv_push(&argv, "-1");
            argv_ok = argv_ok && argv_push(&argv, "-i");
            argv_ok = argv_ok && argv_push(&argv, (char *)options->audio_bg);
        }
        if (options->audio_main != NULL && options->audio_main[0] != '\0')
        {
            main_index = next_input++;
            argv_ok = argv_ok && argv_push(&argv, "-i");
            argv_ok = argv_ok && argv_push(&argv, (char *)options->audio_main);
        }
        if (argv_ok &&
            !build_audio_filter(options, ffmpeg_path, bg_index, main_index,
                                video_dur, audio_filter, sizeof audio_filter))
        {
            argv_free(&argv);
            free(ffmpeg_path);
            free_frame_renderer(&renderer);
            return false;
        }
        argv_ok = argv_ok && argv_push(&argv, "-filter_complex");
        argv_ok = argv_ok && argv_push(&argv, audio_filter);
        argv_ok = argv_ok && argv_push(&argv, "-t");
        argv_ok = argv_ok && argv_push(&argv, duration_arg);
        argv_ok = argv_ok && argv_push(&argv, "-map");
        argv_ok = argv_ok && argv_push(&argv, "0:v");
        argv_ok = argv_ok && argv_push(&argv, "-map");
        argv_ok = argv_ok && argv_push(&argv, "[aout]");
        argv_ok = argv_ok && argv_push(&argv, "-c:a");
        argv_ok = argv_ok && argv_push(&argv, "aac");
        argv_ok = argv_ok && argv_push(&argv, "-b:a");
        argv_ok = argv_ok && argv_push(&argv, "192k");
    }
    else
    {
        argv_ok = argv_ok && argv_push(&argv, "-an");
    }

    argv_ok = argv_ok && argv_push(&argv, "-c:v");
    argv_ok = argv_ok && argv_push(&argv, "libx264");
    argv_ok = argv_ok && argv_push(&argv, "-preset");
    argv_ok = argv_ok && argv_push(&argv, (char *)options->preset);
    argv_ok = argv_ok && argv_push(&argv, "-crf");
    argv_ok = argv_ok && argv_push(&argv, crf_arg);
    argv_ok = argv_ok && argv_push(&argv, "-pix_fmt");
    argv_ok = argv_ok && argv_push(&argv, "yuv420p");
    argv_ok = argv_ok && argv_push(&argv, "-movflags");
    argv_ok = argv_ok && argv_push(&argv, "+faststart");
    argv_ok = argv_ok && argv_push(&argv, (char *)options->out);
    if (!argv_ok)
    {
        fprintf(stderr, "Out of memory building the ffmpeg command.\n");
        argv_free(&argv);
        free(ffmpeg_path);
        free_frame_renderer(&renderer);
        return false;
    }

    /* A dead ffmpeg should surface as a write error, not a fatal signal. */
    signal(SIGPIPE, SIG_IGN);

    int write_fd = -1;
    pid_t pid = spawn_ffmpeg(argv.items, &write_fd);
    argv_free(&argv);
    if (pid < 0)
    {
        fprintf(stderr, "Could not start ffmpeg.\n");
        free(ffmpeg_path);
        free_frame_renderer(&renderer);
        return false;
    }

    /* Bound the frame pool: each frame buffer is large, so a few in flight is
       enough to keep ffmpeg fed without using excessive memory. */
    int batch = cpu_count();
    if (batch > 4)
    {
        batch = 4;
    }
    if (batch < 1)
    {
        batch = 1;
    }

    Image **frames = (Image **)calloc((size_t)batch, sizeof(Image *));
    bool ok = frames != NULL;
    size_t frame_bytes = (size_t)out_w * out_h * 3;
    int progress_step = options->fps * 5;
    if (progress_step < 1)
    {
        progress_step = 1;
    }

    for (int start = 0; ok && start < frame_count; start += batch)
    {
        int n = frame_count - start;
        if (n > batch)
        {
            n = batch;
        }
        BatchContext ctx = {&renderer, &plan, frame_count, start, frames};
        parallel_for(n, batch, render_batch_one, &ctx);

        for (int k = 0; k < n; ++k)
        {
            if (frames[k] == NULL)
            {
                fprintf(stderr, "Out of memory while rendering frame %d.\n",
                        start + k + 1);
                ok = false;
                break;
            }
            if (!write_all(write_fd, frames[k]->data, frame_bytes))
            {
                fprintf(stderr, "ffmpeg stopped while receiving frames.\n");
                image_free(frames[k]);
                frames[k] = NULL;
                ok = false;
                break;
            }
            int index = start + k;
            if (index % progress_step == 0)
            {
                fprintf(stderr, "Rendered frame %d/%d\n", index + 1, frame_count);
            }
            image_free(frames[k]);
            frames[k] = NULL;
        }
    }

    if (frames != NULL)
    {
        for (int k = 0; k < batch; ++k)
        {
            image_free(frames[k]);
        }
        free(frames);
    }

    if (write_fd >= 0)
    {
        close(write_fd);
    }
    int status = 0;
    waitpid(pid, &status, 0);

    free(ffmpeg_path);
    free_frame_renderer(&renderer);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
        fprintf(stderr, "ffmpeg failed with exit code %d.\n", WEXITSTATUS(status));
        return false;
    }
    if (!WIFEXITED(status))
    {
        fprintf(stderr, "ffmpeg terminated abnormally.\n");
        return false;
    }
    return ok;
}
