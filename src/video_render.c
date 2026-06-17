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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "image.h"
#include "parallel.h"

/* Output frame sizes for each supported format. */
typedef struct {
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

/* Normalised anchor points the camera pans between during the tour. */
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

/* One frame's camera mode, normalised centre, and crop zoom. */
typedef struct {
    int is_contain; /* non-zero shows the full collage instead of a crop */
    double cx;
    double cy;
    double zoom;
} CameraState;

/* Cached source imagery and the reusable wide "contain" frame. */
typedef struct {
    Image *source;
    int out_w;
    int out_h;
    Image *contain_frame;
} FrameRenderer;

/* --- small maths helpers ------------------------------------------------ */

static double clampd(double value, double lo, double hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
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
 * across the anchors, and a final pull-back to the full collage.
 */
static CameraState get_camera_state(double progress, int cycles, double max_zoom)
{
    const double open_hold = 0.075;
    const double zoom_in_end = 0.20;
    const double close_start = 0.92;
    double tour_zoom = max_zoom > 1.05 ? max_zoom : 1.05;

    if (progress <= open_hold) {
        return make_state(1, 0.5, 0.5, 1.0);
    }

    if (progress < zoom_in_end) {
        double amount = smoothstep((progress - open_hold) / (zoom_in_end - open_hold));
        double cx = lerp(0.5, kAnchors[1][0], amount);
        double cy = lerp(0.5, kAnchors[1][1], amount);
        double zoom = lerp(1.05, tour_zoom, amount);
        return make_state(0, cx, cy, zoom);
    }

    if (progress >= close_start) {
        double amount = smoothstep((progress - close_start) / (1.0 - close_start));
        int si = (cycles > 1 ? cycles : 1) % kAnchorCount;
        double cx = lerp(kAnchors[si][0], 0.5, amount);
        double cy = lerp(kAnchors[si][1], 0.5, amount);
        double zoom = lerp(tour_zoom, 1.0, amount);
        if (progress >= 0.999) {
            return make_state(1, 0.5, 0.5, 1.0);
        }
        return make_state(0, cx, cy, zoom);
    }

    double tour_progress = (progress - zoom_in_end) / (close_start - zoom_in_end);
    int segment_count = cycles > 1 ? cycles : 1;
    double segment_pos = tour_progress * segment_count;
    double cap = segment_count - 0.000001;
    if (segment_pos > cap) {
        segment_pos = cap;
    }
    int segment_index = (int)segment_pos;
    double segment_progress = smoothstep(segment_pos - segment_index);
    int i0 = (segment_index + 1) % kAnchorCount;
    int i1 = (segment_index + 2) % kAnchorCount;
    double cx = lerp(kAnchors[i0][0], kAnchors[i1][0], segment_progress);
    double cy = lerp(kAnchors[i0][1], kAnchors[i1][1], segment_progress);
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
    if (background == NULL) {
        return NULL;
    }
    gaussian_blur(background, 28.0);

    Image *white = image_new(out_w, out_h, 3);
    if (white == NULL) {
        image_free(background);
        return NULL;
    }
    image_fill(white, 255, 255, 255);
    image_blend(background, white, 0.18);
    image_free(white);

    Image *contained = image_fit_contain(source, out_w, out_h);
    if (contained == NULL) {
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
    if (access(path, F_OK) != 0) {
        fprintf(stderr, "Input image not found: %s\n", path);
        return false;
    }
    Image *source = image_load(path);
    if (source == NULL) {
        return false; /* image_load already reported the reason */
    }
    if (source->width < 2 || source->height < 2) {
        fprintf(stderr, "Input image is too small to render a video.\n");
        image_free(source);
        return false;
    }
    Image *contain_frame = make_contain_frame(source, out_w, out_h);
    if (contain_frame == NULL) {
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
    if (src_aspect >= out_aspect) {
        max_h = src->height;
        max_w = max_h * out_aspect;
    } else {
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
    if (right < left + 1) {
        right = left + 1;
    }
    if (bottom < top + 1) {
        bottom = top + 1;
    }
    return resample_region(src, left, top, right - left, bottom - top,
                           renderer->out_w, renderer->out_h);
}

/* Render a single frame for a camera state. Returns NULL on allocation failure. */
static Image *render_frame(const FrameRenderer *renderer, const CameraState *state)
{
    if (state->is_contain) {
        return image_copy(renderer->contain_frame);
    }
    return crop_frame(renderer, state);
}

/* --- ffmpeg plumbing ---------------------------------------------------- */

/* Resolve an ffmpeg executable from a path or a PATH lookup; caller frees. */
static char *resolve_ffmpeg(const char *binary)
{
    if (strchr(binary, '/') != NULL) {
        if (access(binary, X_OK) == 0) {
            return strdup(binary);
        }
        fprintf(stderr,
                "ffmpeg not found on PATH. Install it or pass `--ffmpeg <path>`.\n");
        return NULL;
    }
    const char *path = getenv("PATH");
    if (path != NULL) {
        char *copy = strdup(path);
        if (copy == NULL) {
            return NULL;
        }
        char *save = NULL;
        for (char *dir = strtok_r(copy, ":", &save); dir != NULL;
             dir = strtok_r(NULL, ":", &save)) {
            char full[4096];
            snprintf(full, sizeof full, "%s/%s", dir, binary);
            if (access(full, X_OK) == 0) {
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

/* Start ffmpeg with its stdin connected to a pipe we write frames into. */
static pid_t spawn_ffmpeg(char *const argv[], int *write_fd)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
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
    while (offset < n) {
        ssize_t written = write(fd, buf + offset, n - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

/* --- validation --------------------------------------------------------- */

static const FormatSize *find_format(const char *name)
{
    for (int i = 0; i < kFormatCount; ++i) {
        if (strcmp(kFormats[i].name, name) == 0) {
            return &kFormats[i];
        }
    }
    return NULL;
}

static bool validate_options(const VideoOptions *o)
{
    if (find_format(o->format) == NULL) {
        fprintf(stderr,
                "Unsupported format `%s`. Use one of: feed, reel, square.\n",
                o->format);
        return false;
    }
    if (o->duration <= 0.0) {
        fprintf(stderr, "--duration must be greater than zero.\n");
        return false;
    }
    if (o->fps <= 0 || o->fps > 120) {
        fprintf(stderr, "--fps must be between 1 and 120.\n");
        return false;
    }
    if (o->cycles <= 0) {
        fprintf(stderr, "--cycles must be greater than zero.\n");
        return false;
    }
    if (o->zoom < 1.0) {
        fprintf(stderr, "--zoom must be at least 1.0.\n");
        return false;
    }
    if (o->crf < 0 || o->crf > 51) {
        fprintf(stderr, "--crf must be between 0 and 51.\n");
        return false;
    }
    return true;
}

/* --- parallel batch rendering ------------------------------------------- */

/* Shared state for rendering one batch of consecutive frames in parallel. */
typedef struct {
    const FrameRenderer *renderer;
    const VideoOptions *options;
    int frame_count;
    int start;
    Image **frames;
} BatchContext;

static void render_batch_one(void *context, int k)
{
    BatchContext *ctx = (BatchContext *)context;
    int index = ctx->start + k;
    int denominator = ctx->frame_count - 1;
    if (denominator < 1) {
        denominator = 1;
    }
    double progress = (double)index / (double)denominator;
    CameraState state =
        get_camera_state(progress, ctx->options->cycles, ctx->options->zoom);
    ctx->frames[k] = render_frame(ctx->renderer, &state);
}

bool render_video(const VideoOptions *options)
{
    if (!validate_options(options)) {
        return false;
    }
    const FormatSize *format = find_format(options->format);
    int out_w = format->width;
    int out_h = format->height;

    FrameRenderer renderer;
    if (!create_frame_renderer(options->image, out_w, out_h, &renderer)) {
        return false;
    }

    char *ffmpeg_path = resolve_ffmpeg(options->ffmpeg);
    if (ffmpeg_path == NULL) {
        free_frame_renderer(&renderer);
        return false;
    }

    int frame_count = (int)py_round(options->duration * options->fps);
    if (frame_count < 1) {
        frame_count = 1;
    }

    char size_arg[32];
    char fps_arg[16];
    char crf_arg[16];
    snprintf(size_arg, sizeof size_arg, "%dx%d", out_w, out_h);
    snprintf(fps_arg, sizeof fps_arg, "%d", options->fps);
    snprintf(crf_arg, sizeof crf_arg, "%d", options->crf);

    char *argv[] = {
        ffmpeg_path,
        "-y",
        "-loglevel", "error",
        "-f", "rawvideo",
        "-vcodec", "rawvideo",
        "-pix_fmt", "rgb24",
        "-s", size_arg,
        "-r", fps_arg,
        "-i", "-",
        "-an",
        "-c:v", "libx264",
        "-preset", (char *)options->preset,
        "-crf", crf_arg,
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        (char *)options->out,
        NULL,
    };

    /* A dead ffmpeg should surface as a write error, not a fatal signal. */
    signal(SIGPIPE, SIG_IGN);

    int write_fd = -1;
    pid_t pid = spawn_ffmpeg(argv, &write_fd);
    if (pid < 0) {
        fprintf(stderr, "Could not start ffmpeg.\n");
        free(ffmpeg_path);
        free_frame_renderer(&renderer);
        return false;
    }

    /* Bound the frame pool: each frame buffer is large, so a few in flight is
       enough to keep ffmpeg fed without using excessive memory. */
    int batch = cpu_count();
    if (batch > 4) {
        batch = 4;
    }
    if (batch < 1) {
        batch = 1;
    }

    Image **frames = (Image **)calloc((size_t)batch, sizeof(Image *));
    bool ok = frames != NULL;
    size_t frame_bytes = (size_t)out_w * out_h * 3;
    int progress_step = options->fps * 5;
    if (progress_step < 1) {
        progress_step = 1;
    }

    for (int start = 0; ok && start < frame_count; start += batch) {
        int n = frame_count - start;
        if (n > batch) {
            n = batch;
        }
        BatchContext ctx = {&renderer, options, frame_count, start, frames};
        parallel_for(n, batch, render_batch_one, &ctx);

        for (int k = 0; k < n; ++k) {
            if (frames[k] == NULL) {
                fprintf(stderr, "Out of memory while rendering frame %d.\n",
                        start + k + 1);
                ok = false;
                break;
            }
            if (!write_all(write_fd, frames[k]->data, frame_bytes)) {
                fprintf(stderr, "ffmpeg stopped while receiving frames.\n");
                image_free(frames[k]);
                frames[k] = NULL;
                ok = false;
                break;
            }
            int index = start + k;
            if (index % progress_step == 0) {
                fprintf(stderr, "Rendered frame %d/%d\n", index + 1, frame_count);
            }
            image_free(frames[k]);
            frames[k] = NULL;
        }
    }

    if (frames != NULL) {
        for (int k = 0; k < batch; ++k) {
            image_free(frames[k]);
        }
        free(frames);
    }

    if (write_fd >= 0) {
        close(write_fd);
    }
    int status = 0;
    waitpid(pid, &status, 0);

    free(ffmpeg_path);
    free_frame_renderer(&renderer);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ffmpeg failed with exit code %d.\n", WEXITSTATUS(status));
        return false;
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "ffmpeg terminated abnormally.\n");
        return false;
    }
    return ok;
}
