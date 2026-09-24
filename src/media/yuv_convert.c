/*
 * yuv_convert.c
 *
 * Conversion routines, tuned for cache friendly access:
 *   - Y plane is written sequentially per row.
 *   - Chroma is computed from two source rows at a time,
 *     halving chroma reads and avoiding a second pass.
 *
 * Integer only, no branches in the inner loops.
 */
#include "yuv_convert.h"

#include <string.h>

void yuyv_to_i420(const uint8_t *src, uint32_t src_stride,
                  uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                  uint32_t width, uint32_t height)
{
    const uint32_t chroma_w = width / 2;
    const uint32_t chroma_h = height / 2;

    /*
     * Luma: straight copy, two Y samples per 4 byte YUYV macropixel.
     */
    for (uint32_t row = 0; row < height; row++) {
        const uint8_t *in = src + (size_t) row * src_stride;
        uint8_t *out = dst_y + (size_t) row * width;

        for (uint32_t x = 0; x < width / 2; x++) {
            uint32_t y0 = in[0];
            uint32_t y1 = in[2];
            out[0] = (uint8_t) y0;
            out[1] = (uint8_t) y1;
            in += 4;
            out += 2;
        }
    }

    /*
     * Chroma: average vertically from two YUYV rows.
     */
    for (uint32_t crow = 0; crow < chroma_h; crow++) {
        const uint8_t *row0 = src + (size_t) (crow * 2) * src_stride;
        const uint8_t *row1 = row0 + src_stride;

        uint8_t *out_u = dst_u + (size_t) crow * chroma_w;
        uint8_t *out_v = dst_v + (size_t) crow * chroma_w;

        for (uint32_t x = 0; x < chroma_w; x++) {
            uint32_t u = (uint32_t) row0[1] + row1[1];
            uint32_t v = (uint32_t) row0[3] + row1[3];

            *out_u++ = (uint8_t) ((u + 1) >> 1);
            *out_v++ = (uint8_t) ((v + 1) >> 1);

            row0 += 4;
            row1 += 4;
        }
    }
}

void i420_copy(const uint8_t *src_y, uint32_t src_y_stride,
               const uint8_t *src_u, const uint8_t *src_v,
               uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
               uint32_t width, uint32_t height)
{
    const uint32_t chroma_w = width / 2;
    const uint32_t chroma_h = height / 2;
    /* Chroma rows are half the luma stride, padding included. */
    const uint32_t src_c_stride = src_y_stride / 2;

    for (uint32_t row = 0; row < height; row++) {
        memcpy(dst_y + (size_t) row * width,
               src_y + (size_t) row * src_y_stride,
               width);
    }
    for (uint32_t crow = 0; crow < chroma_h; crow++) {
        memcpy(dst_u + (size_t) crow * chroma_w,
               src_u + (size_t) crow * src_c_stride,
               chroma_w);
        memcpy(dst_v + (size_t) crow * chroma_w,
               src_v + (size_t) crow * src_c_stride,
               chroma_w);
    }
}
