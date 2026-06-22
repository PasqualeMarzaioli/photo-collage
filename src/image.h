/*
 * Image type and pixel operations for the collage and zoom-tour tools.
 *
 * This module owns everything the Python version delegated to Pillow and
 * pillow-heif: decoding (JPEG, PNG, WebP, HEIC/HEIF), EXIF orientation, high
 * quality Lanczos resampling, Gaussian blur, alpha compositing, rounded corner
 * masks, and saving to PNG, JPEG, or WebP.
 *
 * All colour images use three interleaved 8-bit channels (RGB), matching
 * Pillow's convert("RGB"). Alpha masks use a single 8-bit channel.
 *
 * Author: Pasquale Marzaioli
 */

#ifndef IMAGE_H
#define IMAGE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * An interleaved 8-bit raster. channels is 3 for RGB colour data and 1 for an
 * alpha mask. The pixel buffer is row-major, tightly packed, and owned by the
 * structure; release it with image_free.
 */
typedef struct {
    int width;
    int height;
    int channels;
    uint8_t *data;
} Image;

/*
 * Round half to even, matching Python's built-in round() for the values used
 * in layout and geometry maths. Used so the C layout matches the Python layout.
 */
static inline long py_round(double value)
{
    return (long)nearbyint(value);
}

/* --- allocation --------------------------------------------------------- */

/* Allocate a zero-filled image. Returns NULL on invalid size or out of memory. */
Image *image_new(int width, int height, int channels);

/* Release an image and its pixel buffer. Safe to call with NULL. */
void image_free(Image *image);

/* Return an independent copy of an image, or NULL on failure. */
Image *image_copy(const Image *source);

/* --- input / output ----------------------------------------------------- */

/*
 * Decode an image file into an RGB image, applying EXIF orientation where
 * present. The format is chosen from the file extension (jpg, jpeg, png, webp,
 * heic, heif). Returns NULL and prints a clear message on failure.
 */
Image *image_load(const char *path);

/*
 * Save an RGB image. The encoder is chosen from the file extension: PNG for
 * .png, JPEG (quality 95) for .jpg/.jpeg, WebP (quality 95) for .webp. Returns
 * false and prints a clear message on failure.
 */
bool image_save(const Image *image, const char *path);

/* --- geometry / resampling ---------------------------------------------- */

/*
 * Resample a rectangular region of the source into a new dst_w by dst_h image
 * using a separable Lanczos-3 filter. The region is given in source pixel
 * coordinates and may be fractional; samples are clamped to the source edges.
 * Returns NULL on failure.
 */
Image *resample_region(const Image *source,
                       double crop_x, double crop_y,
                       double crop_w, double crop_h,
                       int dst_w, int dst_h);

/*
 * Scale to completely cover dst_w by dst_h while preserving aspect ratio, then
 * centre-crop the overflow. Equivalent to Pillow's ImageOps.fit with centre
 * anchoring. Returns NULL on failure.
 */
Image *image_fit_cover(const Image *source, int dst_w, int dst_h);

/*
 * Scale down to fit within max_w by max_h while preserving aspect ratio, never
 * enlarging. Equivalent to Pillow's Image.thumbnail. Returns NULL on failure.
 */
Image *image_fit_contain(const Image *source, int max_w, int max_h);

/* --- compositing and filters -------------------------------------------- */

/*
 * Build a single-channel alpha mask of the given size with rounded corners.
 * radius is clamped to half of the shorter side. Returns NULL on failure.
 */
Image *make_rounded_mask(int width, int height, int radius);

/*
 * Composite an RGB tile onto an RGB canvas at (x, y). When mask is non-NULL its
 * per-pixel alpha controls the blend (out = tile*a + canvas*(1-a)); when mask
 * is NULL the tile is copied opaque. Regions outside the canvas are clipped.
 */
void paste_with_alpha(Image *canvas, const Image *tile, const Image *mask, int x, int y);

/* Blur an RGB image in place with a separable Gaussian of the given sigma. */
void gaussian_blur(Image *image, double sigma);

/*
 * Blend an overlay into dst in place: dst = dst*(1-amount) + overlay*amount.
 * Both images must share the same dimensions. Mirrors Pillow's Image.blend.
 */
void image_blend(Image *dst, const Image *overlay, double amount);

/* Fill every pixel of an RGB image with a solid colour. */
void image_fill(Image *image, uint8_t r, uint8_t g, uint8_t b);

#endif /* IMAGE_H */
