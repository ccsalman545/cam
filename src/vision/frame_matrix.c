#define _POSIX_C_SOURCE 200809L

#include "frame_matrix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int valid_frame(const GrayFrame *f)
{
    return f != NULL && f->data != NULL && f->width > 0 && f->height > 0 &&
           f->stride >= f->width;
}

int vision_gray_from_yuyv(const uint8_t *src, uint32_t src_stride,
                          GrayFrame *dst)
{
    if (src == NULL || !valid_frame(dst) || src_stride < dst->width * 2U ||
        (dst->width & 1U) != 0) {
        return -1;
    }

    for (uint32_t y = 0; y < dst->height; y++) {
        const uint8_t *row = src + (size_t) y * src_stride;
        uint8_t *out = dst->data + (size_t) y * dst->stride;
        for (uint32_t x = 0; x < dst->width; x += 2) {
            out[x] = row[x * 2U];
            out[x + 1U] = row[x * 2U + 2U];
        }
    }
    return 0;
}

int vision_gray_from_rgb24(const uint8_t *src, uint32_t src_stride,
                           GrayFrame *dst)
{
    if (src == NULL || !valid_frame(dst) || src_stride < dst->width * 3U) {
        return -1;
    }

    for (uint32_t y = 0; y < dst->height; y++) {
        const uint8_t *row = src + (size_t) y * src_stride;
        uint8_t *out = dst->data + (size_t) y * dst->stride;
        for (uint32_t x = 0; x < dst->width; x++) {
            const uint8_t *p = row + x * 3U;
            /* BT.601 integer luma, rounded and clamped by construction. */
            out[x] = (uint8_t) ((77U * p[0] + 150U * p[1] + 29U * p[2] + 128U) >> 8);
        }
    }
    return 0;
}

void vision_row_profile(const GrayFrame *frame, uint64_t *profile)
{
    if (!valid_frame(frame) || profile == NULL) return;
    for (uint32_t y = 0; y < frame->height; y++) {
        uint64_t sum = 0;
        const uint8_t *row = frame->data + (size_t) y * frame->stride;
        for (uint32_t x = 0; x < frame->width; x++) sum += row[x];
        profile[y] = sum;
    }
}

void vision_column_profile(const GrayFrame *frame, uint64_t *profile)
{
    if (!valid_frame(frame) || profile == NULL) return;
    memset(profile, 0, (size_t) frame->width * sizeof(*profile));
    for (uint32_t y = 0; y < frame->height; y++) {
        const uint8_t *row = frame->data + (size_t) y * frame->stride;
        for (uint32_t x = 0; x < frame->width; x++) profile[x] += row[x];
    }
}

int vision_estimate_offset(const GrayFrame *previous,
                           const GrayFrame *current,
                           int max_dx, int max_dy,
                           int *out_dx, int *out_dy,
                           uint64_t *out_error)
{
    if (!valid_frame(previous) || !valid_frame(current) ||
        previous->width != current->width || previous->height != current->height ||
        max_dx < 0 || max_dy < 0 || out_dx == NULL || out_dy == NULL) return -1;

    uint64_t best = UINT64_MAX;
    int best_x = 0, best_y = 0;
    uint32_t min_overlap_x = previous->width / 4U;
    uint32_t min_overlap_y = previous->height / 4U;

    for (int dy = -max_dy; dy <= max_dy; dy++) {
        for (int dx = -max_dx; dx <= max_dx; dx++) {
            uint32_t x0 = dx < 0 ? (uint32_t) -dx : 0;
            uint32_t x1 = dx > 0 ? previous->width - (uint32_t) dx : previous->width;
            uint32_t y0 = dy < 0 ? (uint32_t) -dy : 0;
            uint32_t y1 = dy > 0 ? previous->height - (uint32_t) dy : previous->height;
            if (x1 <= x0 || y1 <= y0 || x1 - x0 < min_overlap_x || y1 - y0 < min_overlap_y) continue;

            uint64_t error = 0;
            for (uint32_t y = y0; y < y1; y++) {
                const uint8_t *a = previous->data + (size_t) y * previous->stride;
                const uint8_t *b = current->data + (size_t) (y + dy) * current->stride;
                for (uint32_t x = x0; x < x1; x++) {
                    int bx = (int) x + dx;
                    int d = (int) a[x] - (int) b[bx];
                    error += (uint64_t) (d < 0 ? -d : d);
                }
            }
            if (error < best) { best = error; best_x = dx; best_y = dy; }
        }
    }
    if (best == UINT64_MAX) return -1;
    *out_dx = best_x; *out_dy = best_y;
    if (out_error != NULL) *out_error = best;
    return 0;
}

struct VisionMosaic {
    uint32_t width, height;
    uint64_t *sum;
    uint32_t *count;
};

VisionMosaic *vision_mosaic_create(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (size_t) width > SIZE_MAX / height) return NULL;
    size_t n = (size_t) width * height;
    VisionMosaic *m = calloc(1, sizeof(*m));
    if (m == NULL) return NULL;
    m->sum = calloc(n, sizeof(*m->sum));
    m->count = calloc(n, sizeof(*m->count));
    if (m->sum == NULL || m->count == NULL) { free(m->sum); free(m->count); free(m); return NULL; }
    m->width = width; m->height = height;
    return m;
}

void vision_mosaic_destroy(VisionMosaic *m)
{
    if (m == NULL) return;
    free(m->sum); free(m->count); free(m);
}

int vision_mosaic_add(VisionMosaic *m, const GrayFrame *f, int ox, int oy)
{
    if (m == NULL || !valid_frame(f)) return -1;
    for (uint32_t y = 0; y < f->height; y++) {
        int gy = oy + (int) y;
        if (gy < 0 || gy >= (int) m->height) continue;
        const uint8_t *row = f->data + (size_t) y * f->stride;
        for (uint32_t x = 0; x < f->width; x++) {
            int gx = ox + (int) x;
            if (gx < 0 || gx >= (int) m->width) continue;
            size_t i = (size_t) gy * m->width + (size_t) gx;
            if (m->count[i] != UINT32_MAX) { m->sum[i] += row[x]; m->count[i]++; }
        }
    }
    return 0;
}

static uint8_t mosaic_value(const VisionMosaic *m, size_t i)
{
    return m->count[i] == 0 ? 0 : (uint8_t) (m->sum[i] / m->count[i]);
}

int vision_mosaic_write_pgm(const VisionMosaic *m, const char *path)
{
    if (m == NULL || path == NULL) return -1;
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) return -1;
    fprintf(fp, "P5\n%u %u\n255\n", m->width, m->height);
    for (size_t i = 0, n = (size_t) m->width * m->height; i < n; i++) fputc(mosaic_value(m, i), fp);
    int ok = ferror(fp) ? -1 : 0;
    if (fclose(fp) != 0) ok = -1;
    return ok;
}

int vision_mosaic_write_obj(const VisionMosaic *m, const char *path, float scale)
{
    if (m == NULL || path == NULL) return -1;
    FILE *fp = fopen(path, "w");
    if (fp == NULL) return -1;
    for (uint32_t y = 0; y < m->height; y++) for (uint32_t x = 0; x < m->width; x++) {
        size_t i = (size_t) y * m->width + x;
        fprintf(fp, "v %u %u %.6f\n", x, y, scale * mosaic_value(m, i));
    }
    for (uint32_t y = 0; y + 1 < m->height; y++) for (uint32_t x = 0; x + 1 < m->width; x++) {
        uint32_t a = y * m->width + x + 1, b = a + 1, c = a + m->width, d = c + 1;
        fprintf(fp, "f %u %u %u %u\n", a, b, d, c);
    }
    int ok = ferror(fp) ? -1 : 0;
    if (fclose(fp) != 0) ok = -1;
    return ok;
}

uint32_t vision_mosaic_width(const VisionMosaic *m) { return m == NULL ? 0 : m->width; }
uint32_t vision_mosaic_height(const VisionMosaic *m) { return m == NULL ? 0 : m->height; }
