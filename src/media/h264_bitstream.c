/*
 * h264_bitstream.c
 *
 * See h264_bitstream.h.
 */
#include "h264_bitstream.h"

#include <string.h>

/*
 * Iterate the NAL units of an Annex-B buffer. Returns 1 with the body
 * (after the start code) in *nal / *nal_length, 0 at the end.
 */
static int next_nal(const uint8_t *data, size_t length, size_t *cursor,
                    const uint8_t **nal, size_t *nal_length)
{
    size_t i = *cursor;
    size_t body = 0;
    int found = 0;

    for (; i + 3 <= length; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            body = i + 3;
            found = 1;
            break;
        }
    }

    if (!found) {
        *cursor = length;
        return 0;
    }

    size_t end = length;

    for (size_t j = body; j + 3 <= length; j++) {
        if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
            end = j;
            break;
        }
    }

    /* Zeros before the next start code are padding or its 4th byte. */
    while (end > body && data[end - 1] == 0) {
        end--;
    }

    *nal = data + body;
    *nal_length = end - body;
    *cursor = end;

    return 1;
}

void h264_au_scan(const uint8_t *data, size_t length, H264AuInfo *info)
{
    memset(info, 0, sizeof(*info));

    if (data == NULL) {
        return;
    }

    size_t cursor = 0;
    const uint8_t *nal = NULL;
    size_t nal_length = 0;

    while (next_nal(data, length, &cursor, &nal, &nal_length)) {
        if (nal_length == 0) {
            continue;
        }

        int type = nal[0] & 0x1F;

        info->nal_count++;

        if (type == H264_NAL_SPS) {
            info->has_sps = 1;
        } else if (type == H264_NAL_PPS) {
            info->has_pps = 1;
        } else if (type >= H264_NAL_SLICE && type <= H264_NAL_IDR) {
            info->has_slice = 1;
            if (type == H264_NAL_IDR) {
                info->has_idr = 1;
            }
        }
    }
}

static void store(uint8_t *slot, size_t *slot_length,
                  const uint8_t *nal, size_t nal_length)
{
    if (nal_length + 4 > H264_PARAM_MAX) {
        return;     /* not a plausible parameter set: keep the old one */
    }

    slot[0] = 0;
    slot[1] = 0;
    slot[2] = 0;
    slot[3] = 1;
    memcpy(slot + 4, nal, nal_length);
    *slot_length = nal_length + 4;
}

void h264_param_cache_update(H264ParamCache *cache,
                             const uint8_t *data,
                             size_t length)
{
    if (cache == NULL || data == NULL) {
        return;
    }

    size_t cursor = 0;
    const uint8_t *nal = NULL;
    size_t nal_length = 0;

    while (next_nal(data, length, &cursor, &nal, &nal_length)) {
        if (nal_length == 0) {
            continue;
        }

        int type = nal[0] & 0x1F;

        if (type == H264_NAL_SPS) {
            store(cache->sps, &cache->sps_length, nal, nal_length);
        } else if (type == H264_NAL_PPS) {
            store(cache->pps, &cache->pps_length, nal, nal_length);
        }
    }
}

size_t h264_prepend_params(const H264ParamCache *cache,
                           const uint8_t *data,
                           size_t length,
                           uint8_t *out,
                           size_t out_capacity)
{
    if (cache == NULL || cache->sps_length == 0 || cache->pps_length == 0) {
        return 0;
    }

    size_t total = cache->sps_length + cache->pps_length + length;

    if (total > out_capacity) {
        return 0;
    }

    memcpy(out, cache->sps, cache->sps_length);
    memcpy(out + cache->sps_length, cache->pps, cache->pps_length);
    memcpy(out + cache->sps_length + cache->pps_length, data, length);

    return total;
}
