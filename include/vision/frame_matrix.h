/*
 * frame_matrix.h
 *
 * Phase 4/5 CPU-side vision primitives. These routines deliberately operate
 * on caller-owned buffers and contain no camera, WebRTC or UI code:
 *
 *   camera frame -> grayscale matrix -> row/column profiles
 *                -> integer displacement -> accumulated mosaic
 *
 * Coordinates use (x, y), with x increasing right and y increasing down.
 * A positive estimated dx/dy means the matching content moved right/down in
 * the current frame relative to the previous frame.
 */
#ifndef VISION_FRAME_MATRIX_H
#define VISION_FRAME_MATRIX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *data;
} GrayFrame;

/* Convert packed camera formats into an 8-bit grayscale matrix. */
int vision_gray_from_yuyv(const uint8_t *src, uint32_t src_stride,
                          GrayFrame *dst);
int vision_gray_from_rgb24(const uint8_t *src, uint32_t src_stride,
                           GrayFrame *dst);

/* Profiles are allocated by the caller: width entries for columns and height
 * entries for rows. Values are sums, not averages, to avoid floating point. */
void vision_row_profile(const GrayFrame *frame, uint64_t *profile);
void vision_column_profile(const GrayFrame *frame, uint64_t *profile);

/* Brute-force integer translation search. max_* bounds the search window.
 * Returns 0 and writes the best offset, or -1 for invalid input. */
int vision_estimate_offset(const GrayFrame *previous,
                           const GrayFrame *current,
                           int max_dx, int max_dy,
                           int *out_dx, int *out_dy,
                           uint64_t *out_error);

typedef struct VisionMosaic VisionMosaic;

VisionMosaic *vision_mosaic_create(uint32_t width, uint32_t height);
void vision_mosaic_destroy(VisionMosaic *mosaic);

/* Add a frame at an arbitrary signed canvas coordinate. Pixels are averaged
 * when multiple slices overlap. Returns -1 on invalid input. */
int vision_mosaic_add(VisionMosaic *mosaic, const GrayFrame *frame,
                      int origin_x, int origin_y);

/* Export averaged intensity as binary PGM (P5), useful for quick inspection. */
int vision_mosaic_write_pgm(const VisionMosaic *mosaic, const char *path);

/* Export the averaged mosaic as a regular-grid OBJ height surface. */
int vision_mosaic_write_obj(const VisionMosaic *mosaic, const char *path,
                            float height_scale);

uint32_t vision_mosaic_width(const VisionMosaic *mosaic);
uint32_t vision_mosaic_height(const VisionMosaic *mosaic);

#endif
