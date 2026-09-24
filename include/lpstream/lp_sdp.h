/*
 * lp_sdp.h
 *
 * Validation of the browser's SDP answer before it reaches libpeer.
 *
 * libpeer parses a remote description with fixed size buffers and no
 * length checks: every line is copied into a 256 byte stack buffer,
 * candidate fields are read with unbounded sscanf("%s") into 16 to 46
 * byte arrays, the remote candidate table holds 10 entries without a
 * bounds check, and the fingerprint is assumed to follow the literal
 * "a=fingerprint:sha-256 ". A browser answer is untrusted network input,
 * so it is rebuilt here into a form libpeer can parse safely:
 *
 *   - CRLF line endings (libpeer splits on "\r\n" only)
 *   - no line longer than LP_SDP_MAX_LINE, no control characters
 *   - ice-ufrag, ice-pwd and a sha-256 fingerprint present and sane
 *   - a=setup:active (libpeer's offer takes the DTLS server role)
 *   - an accepted video m-line (port not 0)
 *   - only candidates libpeer can use: component 1, UDP, IPv4 or an
 *     mDNS ".local" name, type host/srflx/relay, every field within
 *     libpeer's buffer sizes; at most LP_SDP_MAX_CANDIDATES of them
 *
 * Everything else passes through unchanged.
 */
#ifndef LPSTREAM_LP_SDP_H
#define LPSTREAM_LP_SDP_H

#include <stddef.h>

#define LP_SDP_MAX_INPUT 16384
#define LP_SDP_MAX_LINE 250
#define LP_SDP_MAX_CANDIDATES 8

typedef struct {
    int candidates_kept;
    int candidates_dropped;     /* unusable for libpeer (IPv6, TCP, ...) */
    int mdns_candidates;        /* kept candidates that need mDNS lookup */
} LpSdpAnswerInfo;

/*
 * Rebuild 'answer' into 'out'. Returns 0 on success, -1 with a
 * human readable reason in 'error' when the answer cannot be used.
 */
int lp_sdp_sanitize_answer(const char *answer,
                           char *out,
                           size_t out_size,
                           LpSdpAnswerInfo *info,
                           char *error,
                           size_t error_size);

#endif
