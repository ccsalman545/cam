#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_matrix.h"

int main(void)
{
    enum { W = 32, H = 24 };
    uint8_t a[W * H], b[W * H];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    /* A sparse feature moved right by three pixels and down by two. */
    a[7 * W + 8] = 255; a[8 * W + 9] = 200; a[9 * W + 10] = 180;
    b[9 * W + 11] = 255; b[10 * W + 12] = 200; b[11 * W + 13] = 180;
    GrayFrame previous = { W, H, W, a }, current = { W, H, W, b };
    int dx = 0, dy = 0;
    uint64_t error = 0;
    assert(vision_estimate_offset(&previous, &current, 6, 6, &dx, &dy, &error) == 0);
    assert(dx == 3 && dy == 2 && error == 0);

    uint64_t rows[H], columns[W];
    vision_row_profile(&previous, rows);
    vision_column_profile(&previous, columns);
    assert(rows[7] == 255 && columns[8] == 255);

    VisionMosaic *m = vision_mosaic_create(W + 3, H + 2);
    assert(m != NULL);
    assert(vision_mosaic_add(m, &previous, 0, 0) == 0);
    assert(vision_mosaic_add(m, &current, 3, 2) == 0);
    assert(vision_mosaic_write_pgm(m, "build/test-vision.pgm") == 0);
    assert(vision_mosaic_write_obj(m, "build/test-vision.obj", 1.0f) == 0);
    vision_mosaic_destroy(m);
    puts("vision tests passed");
    return 0;
}
