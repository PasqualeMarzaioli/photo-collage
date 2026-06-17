/*
 * Image type and pixel operations.
 *
 * Decoding uses stb_image for JPEG and PNG, libheif for HEIC/HEIF, and libwebp
 * for WebP. Encoding uses stb_image_write for PNG and JPEG and libwebp for
 * WebP. Resampling, blur, blending, masks, and compositing are implemented
 * here so the result matches the Pillow-based Python version.
 *
 * Author: Pasquale Marzaioli
 */

#define _GNU_SOURCE

#include "image.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <libheif/heif.h>
#include <webp/decode.h>
#include <webp/encode.h>

#include "stb_image.h"
#include "stb_image_write.h"

/* Width of the Lanczos window in source pixels (Lanczos-3). */
#define LANCZOS_A 3.0

/* Print a clear, single-line message to stderr, matching the Python tools. */
static void report(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

/* Round a filtered sample to a clamped 8-bit value. */
static uint8_t clamp8(double value)
{
    if (value <= 0.0) {
        return 0;
    }
    if (value >= 255.0) {
        return 255;
    }
    return (uint8_t)(value + 0.5);
}

/* Return non-zero when path ends with a case-insensitive ".ext". */
static int has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return 0;
    }
    return strcasecmp(dot + 1, ext) == 0;
}

/* --- allocation --------------------------------------------------------- */

Image *image_new(int width, int height, int channels)
{
    if (width <= 0 || height <= 0 || (channels != 1 && channels != 3)) {
        return NULL;
    }
    Image *image = (Image *)malloc(sizeof(Image));
    if (image == NULL) {
        return NULL;
    }
    size_t count = (size_t)width * (size_t)height * (size_t)channels;
    image->data = (uint8_t *)calloc(count, 1);
    if (image->data == NULL) {
        free(image);
        return NULL;
    }
    image->width = width;
    image->height = height;
    image->channels = channels;
    return image;
}

void image_free(Image *image)
{
    if (image == NULL) {
        return;
    }
    free(image->data);
    free(image);
}

Image *image_copy(const Image *source)
{
    if (source == NULL) {
        return NULL;
    }
    Image *copy = image_new(source->width, source->height, source->channels);
    if (copy == NULL) {
        return NULL;
    }
    size_t count = (size_t)source->width * (size_t)source->height * (size_t)source->channels;
    memcpy(copy->data, source->data, count);
    return copy;
}

void image_fill(Image *image, uint8_t r, uint8_t g, uint8_t b)
{
    if (image == NULL || image->channels != 3) {
        return;
    }
    size_t pixels = (size_t)image->width * (size_t)image->height;
    for (size_t i = 0; i < pixels; ++i) {
        image->data[i * 3 + 0] = r;
        image->data[i * 3 + 1] = g;
        image->data[i * 3 + 2] = b;
    }
}

/* --- EXIF orientation (JPEG) -------------------------------------------- */

/* Read a 16-bit value with the given byte order. */
static unsigned read_u16(const uint8_t *p, int little_endian)
{
    return little_endian ? (unsigned)(p[0] | (p[1] << 8))
                         : (unsigned)((p[0] << 8) | p[1]);
}

/* Read a 32-bit value with the given byte order. */
static unsigned read_u32(const uint8_t *p, int little_endian)
{
    return little_endian
               ? (unsigned)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
               : (unsigned)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* Parse the EXIF Orientation tag (0x0112) from a TIFF block; default to 1. */
static int parse_tiff_orientation(const uint8_t *tiff, size_t len)
{
    if (len < 8) {
        return 1;
    }
    int little_endian;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        little_endian = 1;
    } else if (tiff[0] == 'M' && tiff[1] == 'M') {
        little_endian = 0;
    } else {
        return 1;
    }
    unsigned ifd_offset = read_u32(tiff + 4, little_endian);
    if ((size_t)ifd_offset + 2 > len) {
        return 1;
    }
    unsigned entries = read_u16(tiff + ifd_offset, little_endian);
    size_t entry = (size_t)ifd_offset + 2;
    for (unsigned i = 0; i < entries; ++i) {
        if (entry + 12 > len) {
            break;
        }
        unsigned tag = read_u16(tiff + entry, little_endian);
        if (tag == 0x0112) {
            unsigned value = read_u16(tiff + entry + 8, little_endian);
            if (value >= 1 && value <= 8) {
                return (int)value;
            }
            return 1;
        }
        entry += 12;
    }
    return 1;
}

/*
 * Read the EXIF orientation from a JPEG file by scanning its markers for the
 * APP1 "Exif" segment. Only the head of the file is inspected, which always
 * covers the first IFD where the orientation lives. Returns 1 (upright) when no
 * orientation is found.
 */
static int read_jpeg_orientation(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 1;
    }
    static const size_t kMaxScan = 65536;
    uint8_t *buf = (uint8_t *)malloc(kMaxScan);
    if (buf == NULL) {
        fclose(file);
        return 1;
    }
    size_t len = fread(buf, 1, kMaxScan, file);
    fclose(file);

    int orientation = 1;
    if (len >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
        size_t p = 2;
        while (p + 4 <= len) {
            if (buf[p] != 0xFF) {
                p++;
                continue;
            }
            uint8_t marker = buf[p + 1];
            /* Markers without a payload length. */
            if (marker == 0xD8 || marker == 0xD9 ||
                (marker >= 0xD0 && marker <= 0xD7)) {
                p += 2;
                continue;
            }
            unsigned seg_len = (unsigned)((buf[p + 2] << 8) | buf[p + 3]);
            if (seg_len < 2) {
                break;
            }
            size_t payload = p + 4;
            size_t seg_end = p + 2 + seg_len;
            if (marker == 0xE1 && payload + 6 <= len &&
                memcmp(buf + payload, "Exif\0\0", 6) == 0) {
                size_t tiff_len = (seg_end <= len ? seg_end : len) - (payload + 6);
                orientation = parse_tiff_orientation(buf + payload + 6, tiff_len);
                break;
            }
            if (marker == 0xDA) {
                /* Start of scan: pixel data begins, no more metadata. */
                break;
            }
            p = seg_end;
        }
    }
    free(buf);
    return orientation;
}

/*
 * Return a new image with the given EXIF orientation (2..8) applied, mapping
 * each source pixel to its upright position. Dimensions are swapped for the
 * rotations and diagonal flips. Returns NULL on failure.
 */
static Image *transform_oriented(const Image *src, int orientation)
{
    int sw = src->width;
    int sh = src->height;
    int channels = src->channels;
    int swap = (orientation == 5 || orientation == 6 ||
                orientation == 7 || orientation == 8);
    int dw = swap ? sh : sw;
    int dh = swap ? sw : sh;

    Image *out = image_new(dw, dh, channels);
    if (out == NULL) {
        return NULL;
    }
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            int nx;
            int ny;
            switch (orientation) {
            case 2: nx = sw - 1 - x; ny = y;          break; /* mirror */
            case 3: nx = sw - 1 - x; ny = sh - 1 - y; break; /* 180 */
            case 4: nx = x;          ny = sh - 1 - y; break; /* flip */
            case 5: nx = y;          ny = x;          break; /* transpose */
            case 6: nx = sh - 1 - y; ny = x;          break; /* rotate 90 CW */
            case 7: nx = sh - 1 - y; ny = sw - 1 - x; break; /* transverse */
            case 8: nx = y;          ny = sw - 1 - x; break; /* rotate 90 CCW */
            default: nx = x;         ny = y;          break;
            }
            const uint8_t *sp = src->data + ((size_t)y * sw + x) * channels;
            uint8_t *dp = out->data + ((size_t)ny * dw + nx) * channels;
            for (int k = 0; k < channels; ++k) {
                dp[k] = sp[k];
            }
        }
    }
    return out;
}

/* --- decoders ----------------------------------------------------------- */

/* Decode JPEG or PNG via stb_image, always returning three channels. */
static Image *load_stb(const char *path)
{
    int width;
    int height;
    int channels_in_file;
    uint8_t *data = stbi_load(path, &width, &height, &channels_in_file, 3);
    if (data == NULL) {
        report("Could not open image: %s", path);
        return NULL;
    }
    Image *image = (Image *)malloc(sizeof(Image));
    if (image == NULL) {
        stbi_image_free(data);
        return NULL;
    }
    image->width = width;
    image->height = height;
    image->channels = 3;
    image->data = data; /* stb uses the standard allocator; image_free matches */
    return image;
}

/* Decode HEIC/HEIF via libheif. libheif applies stored orientation itself. */
static Image *load_heif(const char *path)
{
    struct heif_context *ctx = heif_context_alloc();
    if (ctx == NULL) {
        report("Could not open image: %s", path);
        return NULL;
    }

    Image *image = NULL;
    struct heif_image_handle *handle = NULL;
    struct heif_image *decoded = NULL;

    struct heif_error err = heif_context_read_from_file(ctx, path, NULL);
    if (err.code != heif_error_Ok) {
        report("Could not open image: %s", path);
        goto cleanup;
    }
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || handle == NULL) {
        report("Could not open image: %s", path);
        goto cleanup;
    }
    err = heif_decode_image(handle, &decoded, heif_colorspace_RGB,
                            heif_chroma_interleaved_RGB, NULL);
    if (err.code != heif_error_Ok || decoded == NULL) {
        report("Could not open image: %s", path);
        goto cleanup;
    }

    int width = heif_image_get_width(decoded, heif_channel_interleaved);
    int height = heif_image_get_height(decoded, heif_channel_interleaved);
    int stride = 0;
    const uint8_t *plane =
        heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
    if (plane != NULL && width > 0 && height > 0) {
        image = image_new(width, height, 3);
        if (image != NULL) {
            for (int y = 0; y < height; ++y) {
                memcpy(image->data + (size_t)y * width * 3,
                       plane + (size_t)y * stride,
                       (size_t)width * 3);
            }
        }
    }
    if (image == NULL) {
        report("Could not open image: %s", path);
    }

cleanup:
    if (decoded != NULL) {
        heif_image_release(decoded);
    }
    if (handle != NULL) {
        heif_image_handle_release(handle);
    }
    heif_context_free(ctx);
    return image;
}

/* Decode WebP via libwebp. */
static Image *load_webp(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        report("Could not open image: %s", path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        report("Could not open image: %s", path);
        return NULL;
    }
    uint8_t *bytes = (uint8_t *)malloc((size_t)size);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(bytes);
        report("Could not open image: %s", path);
        return NULL;
    }
    fclose(file);

    int width;
    int height;
    uint8_t *rgb = WebPDecodeRGB(bytes, (size_t)size, &width, &height);
    free(bytes);
    if (rgb == NULL) {
        report("Could not open image: %s", path);
        return NULL;
    }
    Image *image = image_new(width, height, 3);
    if (image != NULL) {
        memcpy(image->data, rgb, (size_t)width * height * 3);
    }
    WebPFree(rgb);
    return image;
}

Image *image_load(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    if (has_ext(path, "jpg") || has_ext(path, "jpeg")) {
        Image *image = load_stb(path);
        if (image != NULL) {
            int orientation = read_jpeg_orientation(path);
            if (orientation != 1) {
                Image *fixed = transform_oriented(image, orientation);
                if (fixed != NULL) {
                    image_free(image);
                    image = fixed;
                }
            }
        }
        return image;
    }
    if (has_ext(path, "png")) {
        return load_stb(path);
    }
    if (has_ext(path, "webp")) {
        return load_webp(path);
    }
    if (has_ext(path, "heic") || has_ext(path, "heif")) {
        return load_heif(path);
    }
    report("Unsupported image type: %s", path);
    return NULL;
}

/* --- saving ------------------------------------------------------------- */

/* Create every parent directory of path, ignoring those that already exist. */
static void ensure_parent_dirs(const char *path)
{
    char *copy = strdup(path);
    if (copy == NULL) {
        return;
    }
    for (char *p = copy + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(copy, 0755);
            *p = '/';
        }
    }
    free(copy);
}

bool image_save(const Image *image, const char *path)
{
    if (image == NULL || image->channels != 3 || path == NULL) {
        report("Could not save output image: %s", path != NULL ? path : "(null)");
        return false;
    }
    ensure_parent_dirs(path);

    int ok = 0;
    if (has_ext(path, "jpg") || has_ext(path, "jpeg")) {
        ok = stbi_write_jpg(path, image->width, image->height, 3, image->data, 95);
    } else if (has_ext(path, "png")) {
        ok = stbi_write_png(path, image->width, image->height, 3, image->data,
                            image->width * 3);
    } else if (has_ext(path, "webp")) {
        uint8_t *out = NULL;
        size_t out_size = WebPEncodeRGB(image->data, image->width, image->height,
                                        image->width * 3, 95.0f, &out);
        if (out_size > 0 && out != NULL) {
            FILE *file = fopen(path, "wb");
            if (file != NULL) {
                ok = fwrite(out, 1, out_size, file) == out_size;
                fclose(file);
            }
        }
        WebPFree(out);
    } else {
        report("Could not save output image: %s (unsupported extension)", path);
        return false;
    }

    if (!ok) {
        report("Could not save output image: %s", path);
        return false;
    }
    return true;
}

/* --- Lanczos resampling ------------------------------------------------- */

/* Normalised sinc function used by the Lanczos kernel. */
static double sinc_pi(double x)
{
    if (x == 0.0) {
        return 1.0;
    }
    x *= M_PI;
    return sin(x) / x;
}

/* Lanczos-3 kernel. */
static double lanczos(double x)
{
    if (x <= -LANCZOS_A || x >= LANCZOS_A) {
        return 0.0;
    }
    return sinc_pi(x) * sinc_pi(x / LANCZOS_A);
}

/* Per-output-sample filter weights for one axis of a resample. */
typedef struct {
    int *start;       /* first source sample for each output sample */
    int *count;       /* number of source samples for each output sample */
    double *weights;  /* out * taps normalised weights, row-major */
    int taps;         /* maximum source samples per output sample */
    int ok;           /* non-zero when allocation succeeded */
} Coeffs;

static void free_coeffs(Coeffs *c)
{
    free(c->start);
    free(c->count);
    free(c->weights);
    c->start = NULL;
    c->count = NULL;
    c->weights = NULL;
}

/*
 * Build the Lanczos weights mapping a source span [origin, origin + extent)
 * onto out output samples. When downscaling, the filter widens so it averages
 * the right number of source pixels (the same approach Pillow uses); samples
 * are clamped to [0, in_size) and the weights are renormalised at the edges.
 */
static Coeffs build_coeffs(double origin, double extent, int in_size, int out)
{
    Coeffs c;
    c.start = NULL;
    c.count = NULL;
    c.weights = NULL;
    c.taps = 0;
    c.ok = 0;

    double scale = extent / (double)out;
    double filter_scale = scale < 1.0 ? 1.0 : scale;
    double support = LANCZOS_A * filter_scale;
    double inv = 1.0 / filter_scale;

    int taps = (int)ceil(support * 2.0) + 1;
    if (taps < 1) {
        taps = 1;
    }

    c.start = (int *)malloc((size_t)out * sizeof(int));
    c.count = (int *)malloc((size_t)out * sizeof(int));
    c.weights = (double *)malloc((size_t)out * (size_t)taps * sizeof(double));
    if (c.start == NULL || c.count == NULL || c.weights == NULL) {
        free_coeffs(&c);
        return c;
    }
    c.taps = taps;

    for (int i = 0; i < out; ++i) {
        double center = origin + (i + 0.5) * scale;
        int xmin = (int)floor(center - support);
        int xmax = (int)ceil(center + support);
        if (xmin < 0) {
            xmin = 0;
        }
        if (xmax > in_size) {
            xmax = in_size;
        }
        int n = xmax - xmin;
        if (n < 1) {
            /* Degenerate span: fall back to the nearest valid sample. */
            xmin = (int)floor(center);
            if (xmin < 0) {
                xmin = 0;
            }
            if (xmin > in_size - 1) {
                xmin = in_size - 1;
            }
            n = 1;
        }
        if (n > taps) {
            n = taps;
        }
        double sum = 0.0;
        double *row = c.weights + (size_t)i * taps;
        for (int k = 0; k < n; ++k) {
            double w = lanczos((xmin + k + 0.5 - center) * inv);
            row[k] = w;
            sum += w;
        }
        if (sum != 0.0) {
            for (int k = 0; k < n; ++k) {
                row[k] /= sum;
            }
        } else if (n > 0) {
            row[0] = 1.0;
        }
        c.start[i] = xmin;
        c.count[i] = n;
    }
    c.ok = 1;
    return c;
}

Image *resample_region(const Image *source,
                       double crop_x, double crop_y,
                       double crop_w, double crop_h,
                       int dst_w, int dst_h)
{
    if (source == NULL || source->channels != 3 || dst_w <= 0 || dst_h <= 0) {
        return NULL;
    }

    Coeffs hc = build_coeffs(crop_x, crop_w, source->width, dst_w);
    Coeffs vc = build_coeffs(crop_y, crop_h, source->height, dst_h);
    if (!hc.ok || !vc.ok) {
        free_coeffs(&hc);
        free_coeffs(&vc);
        return NULL;
    }

    /* Only the source rows touched by the vertical filter are needed. */
    int row_lo = source->height;
    int row_hi = 0;
    for (int j = 0; j < dst_h; ++j) {
        int s = vc.start[j];
        int e = vc.start[j] + vc.count[j];
        if (s < row_lo) {
            row_lo = s;
        }
        if (e > row_hi) {
            row_hi = e;
        }
    }
    if (row_lo >= row_hi) {
        row_lo = 0;
        row_hi = source->height;
    }
    int rows = row_hi - row_lo;

    float *tmp = (float *)malloc((size_t)rows * dst_w * 3 * sizeof(float));
    if (tmp == NULL) {
        free_coeffs(&hc);
        free_coeffs(&vc);
        return NULL;
    }

    /* Horizontal pass: filter the needed source rows into the intermediate. */
    for (int r = 0; r < rows; ++r) {
        const uint8_t *srow =
            source->data + (size_t)(row_lo + r) * source->width * 3;
        float *trow = tmp + (size_t)r * dst_w * 3;
        for (int i = 0; i < dst_w; ++i) {
            int xs = hc.start[i];
            int n = hc.count[i];
            const double *w = hc.weights + (size_t)i * hc.taps;
            double a0 = 0.0;
            double a1 = 0.0;
            double a2 = 0.0;
            for (int k = 0; k < n; ++k) {
                const uint8_t *p = srow + (size_t)(xs + k) * 3;
                a0 += w[k] * p[0];
                a1 += w[k] * p[1];
                a2 += w[k] * p[2];
            }
            trow[i * 3 + 0] = (float)a0;
            trow[i * 3 + 1] = (float)a1;
            trow[i * 3 + 2] = (float)a2;
        }
    }

    /* Vertical pass: filter the intermediate columns into the destination. */
    Image *dst = image_new(dst_w, dst_h, 3);
    if (dst != NULL) {
        for (int j = 0; j < dst_h; ++j) {
            int ys = vc.start[j] - row_lo;
            int n = vc.count[j];
            const double *w = vc.weights + (size_t)j * vc.taps;
            uint8_t *drow = dst->data + (size_t)j * dst_w * 3;
            for (int i = 0; i < dst_w; ++i) {
                double a0 = 0.0;
                double a1 = 0.0;
                double a2 = 0.0;
                for (int k = 0; k < n; ++k) {
                    const float *p = tmp + ((size_t)(ys + k) * dst_w + i) * 3;
                    a0 += w[k] * p[0];
                    a1 += w[k] * p[1];
                    a2 += w[k] * p[2];
                }
                drow[i * 3 + 0] = clamp8(a0);
                drow[i * 3 + 1] = clamp8(a1);
                drow[i * 3 + 2] = clamp8(a2);
            }
        }
    }

    free(tmp);
    free_coeffs(&hc);
    free_coeffs(&vc);
    return dst;
}

Image *image_fit_cover(const Image *source, int dst_w, int dst_h)
{
    if (source == NULL || dst_w <= 0 || dst_h <= 0) {
        return NULL;
    }
    double src_aspect = (double)source->width / (double)source->height;
    double dst_aspect = (double)dst_w / (double)dst_h;

    double crop_w;
    double crop_h;
    if (src_aspect >= dst_aspect) {
        crop_h = source->height;
        crop_w = crop_h * dst_aspect;
    } else {
        crop_w = source->width;
        crop_h = crop_w / dst_aspect;
    }
    double crop_x = (source->width - crop_w) / 2.0;
    double crop_y = (source->height - crop_h) / 2.0;
    return resample_region(source, crop_x, crop_y, crop_w, crop_h, dst_w, dst_h);
}

Image *image_fit_contain(const Image *source, int max_w, int max_h)
{
    if (source == NULL || max_w <= 0 || max_h <= 0) {
        return NULL;
    }
    double scale = fmin((double)max_w / source->width,
                        (double)max_h / source->height);
    if (scale > 1.0) {
        scale = 1.0;
    }
    int dst_w = (int)py_round(source->width * scale);
    int dst_h = (int)py_round(source->height * scale);
    if (dst_w < 1) {
        dst_w = 1;
    }
    if (dst_h < 1) {
        dst_h = 1;
    }
    return resample_region(source, 0.0, 0.0, source->width, source->height,
                           dst_w, dst_h);
}

/* --- masks, compositing, filters ---------------------------------------- */

Image *make_rounded_mask(int width, int height, int radius)
{
    Image *mask = image_new(width, height, 1);
    if (mask == NULL) {
        return NULL;
    }
    if (radius <= 0) {
        memset(mask->data, 255, (size_t)width * height);
        return mask;
    }
    double r = radius;
    double max_r = fmin(width, height) / 2.0;
    if (r > max_r) {
        r = max_r;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double px = x + 0.5;
            double py = y + 0.5;
            double cx = px < r ? r : (px > width - r ? width - r : px);
            double cy = py < r ? r : (py > height - r ? height - r : py);
            double dx = px - cx;
            double dy = py - cy;
            double dist = sqrt(dx * dx + dy * dy);
            double alpha = r + 0.5 - dist;
            if (alpha < 0.0) {
                alpha = 0.0;
            }
            if (alpha > 1.0) {
                alpha = 1.0;
            }
            mask->data[(size_t)y * width + x] = (uint8_t)py_round(alpha * 255.0);
        }
    }
    return mask;
}

void paste_with_alpha(Image *canvas, const Image *tile, const Image *mask, int x, int y)
{
    if (canvas == NULL || tile == NULL ||
        canvas->channels != 3 || tile->channels != 3) {
        return;
    }
    for (int ty = 0; ty < tile->height; ++ty) {
        int cy = y + ty;
        if (cy < 0 || cy >= canvas->height) {
            continue;
        }
        for (int tx = 0; tx < tile->width; ++tx) {
            int cx = x + tx;
            if (cx < 0 || cx >= canvas->width) {
                continue;
            }
            const uint8_t *sp = tile->data + ((size_t)ty * tile->width + tx) * 3;
            uint8_t *dp = canvas->data + ((size_t)cy * canvas->width + cx) * 3;
            int a = mask != NULL ? mask->data[(size_t)ty * tile->width + tx] : 255;
            if (a >= 255) {
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
            } else if (a > 0) {
                for (int k = 0; k < 3; ++k) {
                    dp[k] = (uint8_t)((sp[k] * a + dp[k] * (255 - a) + 127) / 255);
                }
            }
        }
    }
}

void gaussian_blur(Image *image, double sigma)
{
    if (image == NULL || image->channels != 3 || sigma <= 0.0) {
        return;
    }
    int radius = (int)ceil(sigma * 3.0);
    if (radius < 1) {
        radius = 1;
    }
    int klen = 2 * radius + 1;
    double *kernel = (double *)malloc((size_t)klen * sizeof(double));
    if (kernel == NULL) {
        return;
    }
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        double v = exp(-(double)(i * i) / (2.0 * sigma * sigma));
        kernel[i + radius] = v;
        sum += v;
    }
    for (int i = 0; i < klen; ++i) {
        kernel[i] /= sum;
    }

    int w = image->width;
    int h = image->height;
    uint8_t *tmp = (uint8_t *)malloc((size_t)w * h * 3);
    if (tmp == NULL) {
        free(kernel);
        return;
    }

    /* Horizontal pass into tmp. */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double a0 = 0.0;
            double a1 = 0.0;
            double a2 = 0.0;
            for (int t = -radius; t <= radius; ++t) {
                int sx = x + t;
                if (sx < 0) {
                    sx = 0;
                }
                if (sx >= w) {
                    sx = w - 1;
                }
                const uint8_t *p = image->data + ((size_t)y * w + sx) * 3;
                double k = kernel[t + radius];
                a0 += k * p[0];
                a1 += k * p[1];
                a2 += k * p[2];
            }
            uint8_t *d = tmp + ((size_t)y * w + x) * 3;
            d[0] = clamp8(a0);
            d[1] = clamp8(a1);
            d[2] = clamp8(a2);
        }
    }

    /* Vertical pass back into the image. */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double a0 = 0.0;
            double a1 = 0.0;
            double a2 = 0.0;
            for (int t = -radius; t <= radius; ++t) {
                int sy = y + t;
                if (sy < 0) {
                    sy = 0;
                }
                if (sy >= h) {
                    sy = h - 1;
                }
                const uint8_t *p = tmp + ((size_t)sy * w + x) * 3;
                double k = kernel[t + radius];
                a0 += k * p[0];
                a1 += k * p[1];
                a2 += k * p[2];
            }
            uint8_t *d = image->data + ((size_t)y * w + x) * 3;
            d[0] = clamp8(a0);
            d[1] = clamp8(a1);
            d[2] = clamp8(a2);
        }
    }

    free(tmp);
    free(kernel);
}

void image_blend(Image *dst, const Image *overlay, double amount)
{
    if (dst == NULL || overlay == NULL ||
        dst->width != overlay->width || dst->height != overlay->height ||
        dst->channels != overlay->channels) {
        return;
    }
    double a = amount;
    if (a < 0.0) {
        a = 0.0;
    }
    if (a > 1.0) {
        a = 1.0;
    }
    size_t count = (size_t)dst->width * dst->height * dst->channels;
    for (size_t i = 0; i < count; ++i) {
        double v = dst->data[i] * (1.0 - a) + overlay->data[i] * a;
        dst->data[i] = clamp8(v);
    }
}
