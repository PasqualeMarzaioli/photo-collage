/*
 * Create a collage from local photos.
 *
 * This program reads supported images from the photos folder, places each photo
 * exactly once into a connected vertical 9:16 collage, and can optionally render
 * a zoom-tour video from the generated collage.
 *
 * Author: Pasquale Marzaioli
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "image.h"
#include "parallel.h"
#include "video_render.h"

/* Base story dimensions and the target 9:16 tile aspect. */
#define BASE_WIDTH 1080
#define BASE_HEIGHT 1920
#define DEFAULT_WIDTH 2160
static const double TILE_ASPECT = 9.0 / 16.0;

/* All settings needed to render a collage and an optional video. Strings are
   borrowed from argv and remain valid for the run. */
typedef struct
{
    const char *photos_dir;
    const char *out;
    int cols;   /* 0 means choose automatically */
    int width;  /* resolved output width in pixels */
    int radius; /* corner radius in base 1080 px units */
    int gap;    /* gap in base 1080 px units */
    uint8_t bg[3];
    int shuffle;
    unsigned long seed;
    int video;
    const char *video_out;
    double video_duration;
    int video_fps;
    int video_cycles;
    double video_zoom;
    const char *video_tour;
    double video_pan_speed;
    const char *ffmpeg;
    const char *audio_main;
    const char *audio_bg;
    double audio_main_vol;
    double audio_bg_low;
    double audio_bg_high;
    double audio_fade;
} CollageOptions;

/* Final canvas and no-repeat row layout in output pixels. */
typedef struct
{
    int width;
    int height;
    int cols;
    int rows;
    int *row_counts; /* length rows; sums to the photo count */
    int gap;
    int radius;
    uint8_t bg[3];
} Layout;

/* --- colour parsing ----------------------------------------------------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* Parse a hex colour string (#rgb or #rrggbb) into RGB. Returns false on error. */
static bool parse_color(const char *value, uint8_t out[3])
{
    const char *t = value;
    if (*t == '#')
    {
        t++;
    }
    char buf[7];
    size_t len = strlen(t);
    if (len == 3)
    {
        for (int i = 0; i < 3; ++i)
        {
            buf[2 * i] = t[i];
            buf[2 * i + 1] = t[i];
        }
        buf[6] = '\0';
        t = buf;
        len = 6;
    }
    if (len != 6)
    {
        fprintf(stderr, "Invalid color `%s`. Use a value like #ffffff.\n", value);
        return false;
    }
    for (int i = 0; i < 3; ++i)
    {
        int hi = hex_value(t[2 * i]);
        int lo = hex_value(t[2 * i + 1]);
        if (hi < 0 || lo < 0)
        {
            fprintf(stderr, "Invalid color `%s`. Use a value like #ffffff.\n", value);
            return false;
        }
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

/* --- layout maths ------------------------------------------------------- */

/* Choose a column count that keeps cells close to 9:16 story tiles. */
static int choose_auto_columns(int photo_count)
{
    int best_cols = 1;
    double best_score = INFINITY;
    for (int cols = 1; cols <= photo_count; ++cols)
    {
        int rows = (int)py_round((double)photo_count / cols);
        if (rows < 1)
        {
            rows = 1;
        }
        double cell_ratio =
            ((double)BASE_WIDTH / cols) / ((double)BASE_HEIGHT / rows);
        double score = fabs(log(cell_ratio / TILE_ASPECT));
        if (score < best_score)
        {
            best_score = score;
            best_cols = cols;
        }
    }
    return best_cols;
}

/*
 * Distribute all photos across balanced rows with no repeated or blank cells.
 * Returns a malloc'd array of length *out_rows, or NULL on error.
 */
static int *distribute_row_counts(int photo_count, int desired_cols, int *out_rows)
{
    int rows = (int)py_round((double)photo_count / desired_cols);
    if (rows < 1)
    {
        rows = 1;
    }
    int *row_counts = (int *)malloc((size_t)rows * sizeof(int));
    if (row_counts == NULL)
    {
        return NULL;
    }
    for (int r = 0; r < rows; ++r)
    {
        row_counts[r] = ((r + 1) * photo_count) / rows - (r * photo_count) / rows;
        if (row_counts[r] <= 0)
        {
            fprintf(stderr, "Could not create a valid no-repeat row layout.\n");
            free(row_counts);
            return NULL;
        }
    }
    *out_rows = rows;
    return row_counts;
}

/* Build the final canvas and no-repeat row layout. Returns false on error. */
static bool build_layout(int photo_count, const CollageOptions *options, Layout *layout)
{
    int base_cols = options->cols > 0 ? options->cols : choose_auto_columns(photo_count);
    int rows = 0;
    int *row_counts = distribute_row_counts(photo_count, base_cols, &rows);
    if (row_counts == NULL)
    {
        return false;
    }

    int width = options->width;
    if (width <= 0)
    {
        fprintf(stderr, "--width must be greater than zero.\n");
        free(row_counts);
        return false;
    }
    int height = (int)py_round((double)width * BASE_HEIGHT / BASE_WIDTH);
    double scale = (double)width / BASE_WIDTH;
    int gap = (int)py_round(options->gap * scale);
    if (gap < 0)
    {
        gap = 0;
    }
    int radius = (int)py_round(options->radius * scale);
    if (radius < 0)
    {
        radius = 0;
    }
    int cols = 0;
    for (int r = 0; r < rows; ++r)
    {
        if (row_counts[r] > cols)
        {
            cols = row_counts[r];
        }
    }
    if (width - gap * (cols + 1) < cols || height - gap * (rows + 1) < rows)
    {
        fprintf(stderr, "Gap is too large for the selected width and grid.\n");
        free(row_counts);
        return false;
    }

    layout->width = width;
    layout->height = height;
    layout->cols = cols;
    layout->rows = rows;
    layout->row_counts = row_counts;
    layout->gap = gap;
    layout->radius = radius;
    memcpy(layout->bg, options->bg, 3);
    return true;
}

/* Return the pixel box [left, top, right, bottom] for a no-repeat cell index. */
static bool tile_box(const Layout *layout, int cell_index, int box[4])
{
    int remaining = cell_index;
    int row = -1;
    for (int ri = 0; ri < layout->rows; ++ri)
    {
        if (remaining < layout->row_counts[ri])
        {
            row = ri;
            break;
        }
        remaining -= layout->row_counts[ri];
    }
    if (row < 0)
    {
        fprintf(stderr, "Cell index out of range: %d\n", cell_index);
        return false;
    }
    int col = remaining;
    int row_count = layout->row_counts[row];
    double tile_w =
        ((double)layout->width - (double)layout->gap * (row_count + 1)) / row_count;
    double tile_h =
        ((double)layout->height - (double)layout->gap * (layout->rows + 1)) / layout->rows;
    int left = (int)py_round(layout->gap + col * (tile_w + layout->gap));
    int top = (int)py_round(layout->gap + row * (tile_h + layout->gap));
    int right = (int)py_round(layout->gap + col * (tile_w + layout->gap) + tile_w);
    int bottom = (int)py_round(layout->gap + row * (tile_h + layout->gap) + tile_h);
    box[0] = left;
    box[1] = top;
    box[2] = right > left + 1 ? right : left + 1;
    box[3] = bottom > top + 1 ? bottom : top + 1;
    return true;
}

/* --- gathering source photos -------------------------------------------- */

static int is_supported_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
    {
        return 0;
    }
    const char *ext = dot + 1;
    static const char *supported[] = {"jpg", "jpeg", "png", "webp", "heic", "heif"};
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i)
    {
        if (strcasecmp(ext, supported[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* Case-insensitive path comparison; paths share a directory so this orders by
   the lowercased file name, matching the Python sort. */
static int compare_paths(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    return strcasecmp(pa, pb);
}

/*
 * Return supported image files from a directory in stable name order. The
 * result is a malloc'd array of malloc'd path strings of length *out_count, or
 * NULL on error.
 */
static char **collect_image_paths(const char *directory, int *out_count)
{
    DIR *dir = opendir(directory);
    if (dir == NULL)
    {
        fprintf(stderr, "Photos directory not found: %s\n", directory);
        return NULL;
    }

    size_t capacity = 64;
    size_t count = 0;
    char **paths = (char **)malloc(capacity * sizeof(char *));
    if (paths == NULL)
    {
        closedir(dir);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
        {
            continue;
        }
        if (!is_supported_extension(entry->d_name))
        {
            continue;
        }
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", directory, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }
        if (count == capacity)
        {
            capacity *= 2;
            char **grown = (char **)realloc(paths, capacity * sizeof(char *));
            if (grown == NULL)
            {
                break;
            }
            paths = grown;
        }
        paths[count] = strdup(full);
        if (paths[count] == NULL)
        {
            break;
        }
        count++;
    }
    closedir(dir);

    if (count == 0)
    {
        fprintf(stderr,
                "No images found in %s/ (supported: heic, heif, jpeg, jpg, png, webp)\n",
                directory);
        free(paths);
        return NULL;
    }
    qsort(paths, count, sizeof(char *), compare_paths);
    *out_count = (int)count;
    return paths;
}

static void free_paths(char **paths, int count)
{
    if (paths == NULL)
    {
        return;
    }
    for (int i = 0; i < count; ++i)
    {
        free(paths[i]);
    }
    free(paths);
}

/* Deterministically shuffle the paths in place using a seeded SplitMix64 PRNG. */
static void shuffle_paths(char **paths, int count, unsigned long seed)
{
    uint64_t state = (uint64_t)seed;
    for (int i = count - 1; i > 0; --i)
    {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        int j = (int)(z % (uint64_t)(i + 1));
        char *tmp = paths[i];
        paths[i] = paths[j];
        paths[j] = tmp;
    }
}

/* --- parallel tile rendering -------------------------------------------- */

/* Shared state for rendering every tile of the collage in parallel. */
typedef struct
{
    char **paths;
    const Layout *layout;
    Image *canvas;
    atomic_int error;
} RenderContext;

/*
 * Render one tile: load the source photo, crop-and-resize it to cover its cell,
 * apply an optional rounded mask, and composite it onto the shared canvas. Tiles
 * occupy disjoint regions of the canvas, so no locking is needed.
 */
static void render_tile(void *context, int index)
{
    RenderContext *ctx = (RenderContext *)context;
    if (atomic_load(&ctx->error))
    {
        return;
    }
    int box[4];
    if (!tile_box(ctx->layout, index, box))
    {
        atomic_store(&ctx->error, 1);
        return;
    }
    int w = box[2] - box[0];
    int h = box[3] - box[1];

    Image *source = image_load(ctx->paths[index]);
    if (source == NULL)
    {
        atomic_store(&ctx->error, 1);
        return;
    }
    Image *tile = image_fit_cover(source, w, h);
    image_free(source);
    if (tile == NULL)
    {
        atomic_store(&ctx->error, 1);
        return;
    }

    Image *mask = NULL;
    if (ctx->layout->radius > 0)
    {
        mask = make_rounded_mask(w, h, ctx->layout->radius);
        if (mask == NULL)
        {
            image_free(tile);
            atomic_store(&ctx->error, 1);
            return;
        }
    }
    paste_with_alpha(ctx->canvas, tile, mask, box[0], box[1]);
    image_free(tile);
    image_free(mask);
}

/* --- orchestration ------------------------------------------------------ */

/* Render the optional zoom-tour video after the collage. Returns false on error. */
static bool render_optional_video(const CollageOptions *options)
{
    VideoOptions video = {
        .image = options->out,
        .out = options->video_out,
        .format = "reel",
        .duration = options->video_duration,
        .fps = options->video_fps,
        .cycles = options->video_cycles,
        .zoom = options->video_zoom,
        .ffmpeg = options->ffmpeg,
        .crf = 18,
        .preset = "medium",
        .tour = options->video_tour,
        .pan_speed = options->video_pan_speed,
        .audio_main = options->audio_main,
        .audio_bg = options->audio_bg,
        .audio_main_vol = options->audio_main_vol,
        .audio_bg_low = options->audio_bg_low,
        .audio_bg_high = options->audio_bg_high,
        .audio_fade = options->audio_fade,
    };
    if (!render_video(&video))
    {
        fprintf(stderr, "Video rendering failed.\n");
        return false;
    }
    return true;
}

/* Render the requested collage (and optional video). Returns a process code. */
static int render_collage(const CollageOptions *options)
{
    int count = 0;
    char **paths = collect_image_paths(options->photos_dir, &count);
    if (paths == NULL)
    {
        return 1;
    }
    if (options->shuffle)
    {
        shuffle_paths(paths, count, options->seed);
    }

    Layout layout;
    if (!build_layout(count, options, &layout))
    {
        free_paths(paths, count);
        return 1;
    }

    Image *canvas = image_new(layout.width, layout.height, 3);
    if (canvas == NULL)
    {
        fprintf(stderr, "Out of memory allocating the canvas.\n");
        free(layout.row_counts);
        free_paths(paths, count);
        return 1;
    }
    image_fill(canvas, layout.bg[0], layout.bg[1], layout.bg[2]);

    RenderContext ctx;
    ctx.paths = paths;
    ctx.layout = &layout;
    ctx.canvas = canvas;
    atomic_init(&ctx.error, 0);
    parallel_for(count, cpu_count(), render_tile, &ctx);

    int status = 0;
    if (atomic_load(&ctx.error))
    {
        status = 1;
    }
    else
    {
        if (!image_save(canvas, options->out))
        {
            status = 1;
        }
        fprintf(stderr, "Rendered %d photos as %dx%d with %d rows.\n",
                count, layout.width, layout.height, layout.rows);
    }

    image_free(canvas);
    free(layout.row_counts);
    free_paths(paths, count);

    if (status == 0 && options->video)
    {
        if (!render_optional_video(options))
        {
            status = 1;
        }
    }
    return status;
}

/* --- command line ------------------------------------------------------- */

enum
{
    OPT_PHOTOS = 1000,
    OPT_OUT,
    OPT_MODE,
    OPT_COLS,
    OPT_SCALE,
    OPT_WIDTH,
    OPT_RADIUS,
    OPT_GAP,
    OPT_BG,
    OPT_SHUFFLE,
    OPT_SEED,
    OPT_VIDEO,
    OPT_VIDEO_OUT,
    OPT_DURATION,
    OPT_FPS,
    OPT_CYCLES,
    OPT_ZOOM,
    OPT_TOUR,
    OPT_PAN_SPEED,
    OPT_FFMPEG,
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
            "Render a 9:16 Instagram Stories collage.\n"
            "\n"
            "Usage: collage [options]\n"
            "  --photos DIR       Directory containing photos (default: photos)\n"
            "  --out PATH         Output image path (default: collage.png)\n"
            "  --cols N           Approximate column count, or omit for auto\n"
            "  --scale F          Output scale relative to 1080 px width\n"
            "  --width N          Output image width in pixels (default: 2160)\n"
            "  --radius N         Corner radius in base 1080 px units\n"
            "  --gap N            Gap in base 1080 px units\n"
            "  --bg COLOR         Canvas background color, such as #ffffff\n"
            "  --shuffle          Shuffle source photos deterministically\n"
            "  --seed N           Shuffle seed\n"
            "  --video            Render a Reel MP4 after the collage\n"
            "  --video-out PATH   Output path for --video (default: tour.mp4)\n"
            "  --duration S       Video duration in seconds (default: 50)\n"
            "  --fps N            Video frames per second (default: 30)\n"
            "  --cycles N         Video pan segments, classic tour only (default: 4)\n"
            "  --zoom F           Video maximum zoom (default: 2.8)\n"
            "  --tour NAME        Camera tour: cover (shows all photos) or classic\n"
            "                     (default: cover)\n"
            "  --pan-speed F      Camera speed between photos, 1.0 = normal (default: 1.0)\n"
            "  --ffmpeg PATH      ffmpeg binary or absolute path\n"
            "  --audio-main PATH  Main audio track path for --video\n"
            "  --audio-bg PATH    Background audio track path for --video\n"
            "  --audio-main-vol F Main track gain for --video (default: 1.0)\n"
            "  --audio-bg-low F   Background gain while main plays (default: 0.15)\n"
            "  --audio-bg-high F  Background gain after main ends (default: 1.0)\n"
            "  --audio-fade F     Audio rise and ending fade seconds (default: 1.5)\n");
}

/* Parse an integer option value; on error print a message and return false. */
static bool parse_int(const char *name, const char *text, long *out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0)
    {
        fprintf(stderr, "Invalid integer for %s: %s\n", name, text);
        return false;
    }
    *out = value;
    return true;
}

/* Parse a floating-point option value; on error print a message and return false. */
static bool parse_double(const char *name, const char *text, double *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || *end != '\0' || errno != 0)
    {
        fprintf(stderr, "Invalid number for %s: %s\n", name, text);
        return false;
    }
    *out = value;
    return true;
}

int main(int argc, char **argv)
{
    CollageOptions options;
    options.photos_dir = "photos";
    options.out = "collage.png";
    options.cols = 0;
    options.width = 0;
    options.radius = 0;
    options.gap = 0;
    options.bg[0] = 255;
    options.bg[1] = 255;
    options.bg[2] = 255;
    options.shuffle = 0;
    options.seed = 0;
    options.video = 0;
    options.video_out = "tour.mp4";
    options.video_duration = 50.0;
    options.video_fps = 30;
    options.video_cycles = 4;
    options.video_zoom = 2.8;
    options.video_tour = "cover";
    options.video_pan_speed = 1.0;
    options.ffmpeg = "ffmpeg";
    options.audio_main = NULL;
    options.audio_bg = NULL;
    options.audio_main_vol = 1.0;
    options.audio_bg_low = 0.15;
    options.audio_bg_high = 1.0;
    options.audio_fade = 1.5;

    int width_arg = -1;     /* unset */
    double scale_arg = NAN; /* unset */
    const char *bg_arg = "#ffffff";

    static struct option long_opts[] = {
        {"photos", required_argument, 0, OPT_PHOTOS},
        {"out", required_argument, 0, OPT_OUT},
        {"mode", required_argument, 0, OPT_MODE},
        {"cols", required_argument, 0, OPT_COLS},
        {"scale", required_argument, 0, OPT_SCALE},
        {"width", required_argument, 0, OPT_WIDTH},
        {"radius", required_argument, 0, OPT_RADIUS},
        {"gap", required_argument, 0, OPT_GAP},
        {"bg", required_argument, 0, OPT_BG},
        {"shuffle", no_argument, 0, OPT_SHUFFLE},
        {"seed", required_argument, 0, OPT_SEED},
        {"video", no_argument, 0, OPT_VIDEO},
        {"video-out", required_argument, 0, OPT_VIDEO_OUT},
        {"duration", required_argument, 0, OPT_DURATION},
        {"fps", required_argument, 0, OPT_FPS},
        {"cycles", required_argument, 0, OPT_CYCLES},
        {"zoom", required_argument, 0, OPT_ZOOM},
        {"tour", required_argument, 0, OPT_TOUR},
        {"pan-speed", required_argument, 0, OPT_PAN_SPEED},
        {"ffmpeg", required_argument, 0, OPT_FFMPEG},
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
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1)
    {
        switch (c)
        {
        case OPT_PHOTOS:
            options.photos_dir = optarg;
            break;
        case OPT_OUT:
            options.out = optarg;
            break;
        case OPT_MODE:
            break; /* `collage` and `grid` render identically */
        case OPT_COLS:
            if (!parse_int("--cols", optarg, &iv))
                return 2;
            options.cols = (int)iv;
            break;
        case OPT_SCALE:
            if (!parse_double("--scale", optarg, &scale_arg))
                return 2;
            break;
        case OPT_WIDTH:
            if (!parse_int("--width", optarg, &iv))
                return 2;
            width_arg = (int)iv;
            break;
        case OPT_RADIUS:
            if (!parse_int("--radius", optarg, &iv))
                return 2;
            options.radius = (int)iv;
            break;
        case OPT_GAP:
            if (!parse_int("--gap", optarg, &iv))
                return 2;
            options.gap = (int)iv;
            break;
        case OPT_BG:
            bg_arg = optarg;
            break;
        case OPT_SHUFFLE:
            options.shuffle = 1;
            break;
        case OPT_SEED:
            if (!parse_int("--seed", optarg, &iv))
                return 2;
            options.seed = (unsigned long)iv;
            break;
        case OPT_VIDEO:
            options.video = 1;
            break;
        case OPT_VIDEO_OUT:
            options.video_out = optarg;
            break;
        case OPT_DURATION:
            if (!parse_double("--duration", optarg, &dv))
                return 2;
            options.video_duration = dv;
            break;
        case OPT_FPS:
            if (!parse_int("--fps", optarg, &iv))
                return 2;
            options.video_fps = (int)iv;
            break;
        case OPT_CYCLES:
            if (!parse_int("--cycles", optarg, &iv))
                return 2;
            options.video_cycles = (int)iv;
            break;
        case OPT_ZOOM:
            if (!parse_double("--zoom", optarg, &dv))
                return 2;
            options.video_zoom = dv;
            break;
        case OPT_TOUR:
            options.video_tour = optarg;
            break;
        case OPT_PAN_SPEED:
            if (!parse_double("--pan-speed", optarg, &dv))
                return 2;
            options.video_pan_speed = dv;
            break;
        case OPT_FFMPEG:
            options.ffmpeg = optarg;
            break;
        case OPT_AUDIO_MAIN:
            options.audio_main = optarg;
            break;
        case OPT_AUDIO_BG:
            options.audio_bg = optarg;
            break;
        case OPT_AUDIO_MAIN_VOL:
            if (!parse_double("--audio-main-vol", optarg, &dv))
                return 2;
            options.audio_main_vol = dv;
            break;
        case OPT_AUDIO_BG_LOW:
            if (!parse_double("--audio-bg-low", optarg, &dv))
                return 2;
            options.audio_bg_low = dv;
            break;
        case OPT_AUDIO_BG_HIGH:
            if (!parse_double("--audio-bg-high", optarg, &dv))
                return 2;
            options.audio_bg_high = dv;
            break;
        case OPT_AUDIO_FADE:
            if (!parse_double("--audio-fade", optarg, &dv))
                return 2;
            options.audio_fade = dv;
            break;
        case OPT_HELP:
            print_usage();
            return 0;
        default:
            return 2;
        }
    }

    /* Resolve the output width: --scale wins, then --width, then the default. */
    if (!isnan(scale_arg))
    {
        options.width = (int)py_round((double)BASE_WIDTH * scale_arg);
    }
    else if (width_arg >= 0)
    {
        options.width = width_arg;
    }
    else
    {
        options.width = DEFAULT_WIDTH;
    }

    if (!parse_color(bg_arg, options.bg))
    {
        return 1;
    }

    return render_collage(&options);
}
