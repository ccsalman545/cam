/*
 * h264_bitstream.h
 *
 * Minimal Annex-B inspection shared by the encoder backends and the
 * encode worker. Nothing here decodes slice data: only NAL unit types
 * are looked at, which is all that is needed to
 *   - tell a header-only buffer (SPS/PPS) from a picture,
 *   - know whether an access unit starts a GOP (IDR),
 *   - guarantee that every IDR sent to a viewer carries SPS and PPS.
 */
#ifndef MEDIA_H264_BITSTREAM_H
#define MEDIA_H264_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

#define H264_NAL_SLICE 1
#define H264_NAL_IDR 5
#define H264_NAL_SEI 6
#define H264_NAL_SPS 7
#define H264_NAL_PPS 8
#define H264_NAL_AUD 9

typedef struct {
    int nal_count;
    int has_sps;
    int has_pps;
    int has_idr;
    int has_slice;      /* any VCL NAL unit (types 1..5) */
    int slice_count;    /* VCL NAL units, i.e. slices of the picture */
} H264AuInfo;

/* Summarize the NAL unit types of an Annex-B buffer. */
void h264_au_scan(const uint8_t *data, size_t length, H264AuInfo *info);

/*
 * Last SPS and PPS seen, so an IDR that arrives without them (some
 * hardware encoders emit the headers once, or as a separate buffer)
 * can be completed before it is sent.
 */
#define H264_PARAM_MAX 256

typedef struct {
    uint8_t sps[H264_PARAM_MAX];
    size_t sps_length;          /* including the 4 byte start code */
    uint8_t pps[H264_PARAM_MAX];
    size_t pps_length;
} H264ParamCache;

/* Store every SPS/PPS found in 'data' (Annex-B). */
void h264_param_cache_update(H264ParamCache *cache,
                             const uint8_t *data,
                             size_t length);

/*
 * Write cached SPS + PPS followed by 'data' into 'out'. Returns the
 * total length, or 0 when the cache is incomplete or 'out' too small.
 */
size_t h264_prepend_params(const H264ParamCache *cache,
                           const uint8_t *data,
                           size_t length,
                           uint8_t *out,
                           size_t out_capacity);

#endif
