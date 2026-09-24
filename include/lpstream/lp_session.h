/*
 * lp_session.h
 *
 * One browser viewer served through libpeer.
 *
 * Threading: libpeer's PeerConnection is not thread safe, so each
 * session owns a thread that is the only caller into its
 * PeerConnection once it runs. The HTTP thread creates the connection
 * and its offer before that thread starts, then only hands over the
 * browser's answer and a close request through the session's mailbox.
 * The media thread hands over encoded access units the same way.
 *
 * Negotiation: the Pi offers, the browser answers. libpeer's answer
 * path only includes video when the offer carries an a=ssrc line for
 * it, which a receive-only browser offer never has; the offer path
 * always does. The browser's ICE candidates travel inside its answer:
 * libpeer only pairs remote candidates while applying the remote
 * description, so trickled candidates sent afterwards would never be
 * checked.
 *
 * Media: libpeer stamps RTP at a fixed 90 kHz / 30 fps step per frame
 * and treats every slice NAL unit as one frame, so the encoder must
 * emit single-slice pictures (H264_ENCODER_SINGLE_SLICE). A viewer
 * starts, and restarts after any loss, on an IDR frame.
 */
#ifndef LPSTREAM_LP_SESSION_H
#define LPSTREAM_LP_SESSION_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Encoded access unit shared by every session (reference counted). */
typedef struct {
    atomic_int refs;
    int is_idr;
    uint64_t pts_us;
    size_t size;
    uint8_t data[];
} LpFrame;

LpFrame *lp_frame_create(const uint8_t *data, size_t size, int is_idr,
                         uint64_t pts_us);
void lp_frame_ref(LpFrame *frame);
void lp_frame_unref(LpFrame *frame);

typedef struct LpSession LpSession;

/* Give up on a viewer that never answered or never connected. */
#define LP_SESSION_ANSWER_TIMEOUT_MS 15000
#define LP_SESSION_CONNECT_TIMEOUT_MS 20000

/*
 * Frames waiting for the session thread. When it falls this far
 * behind, the queue is flushed and the viewer resumes on the next IDR:
 * a live view never builds up delay.
 */
#define LP_SESSION_MAILBOX 8

typedef struct {
    uint32_t id;
    const char *state;          /* libpeer state name */
    int connected;
    int finished;
    uint64_t age_ms;
    uint64_t connect_ms;        /* answer -> connected, 0 until connected */
    uint64_t frames_sent;
    uint64_t bytes_sent;
    uint64_t frames_dropped;    /* queue overflow or waiting for an IDR */
    uint64_t keyframe_requests; /* PLI/FIR from the browser */
    uint64_t multi_slice_frames;
    int candidates;             /* remote candidates in the answer */
    char end_reason[128];
} LpSessionStats;

/*
 * Create the PeerConnection, generate the offer (host candidates are
 * gathered synchronously) and start the session thread. 'force_idr' is
 * the encoder's keyframe request flag. Returns NULL with a message in
 * 'error' on failure.
 */
LpSession *lp_session_create(uint32_t id, atomic_int *force_idr,
                             char *error, size_t error_size);

uint32_t lp_session_id(const LpSession *session);

/* The offer SDP, valid for the lifetime of the session. */
const char *lp_session_offer(const LpSession *session);

/*
 * Queue the browser's answer, already checked by
 * lp_sdp_sanitize_answer(). Returns 0, or -1 when the session already
 * has an answer or has ended.
 */
int lp_session_set_answer(LpSession *session, const char *sdp,
                          int candidates);

/* Media thread: queue one access unit (the session takes a reference). */
void lp_session_push_frame(LpSession *session, LpFrame *frame);

/* Media thread: access units were lost; resume on the next IDR. */
void lp_session_resync(LpSession *session);

/* Ask the session thread to end. */
void lp_session_close(LpSession *session);

/* 1 once the session thread has finished and released libpeer. */
int lp_session_finished(const LpSession *session);

void lp_session_get_stats(const LpSession *session, LpSessionStats *out);

/* Stop the thread if still running, join it and free the session. */
void lp_session_destroy(LpSession *session);

#endif
