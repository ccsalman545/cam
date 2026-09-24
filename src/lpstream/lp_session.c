#define _POSIX_C_SOURCE 200809L

/*
 * lp_session.c
 *
 * One viewer: a libpeer PeerConnection driven by its own thread.
 * See lp_session.h for the threading and negotiation rules.
 */
#include "lp_session.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <peer_connection.h>

#include "h264_bitstream.h"
#include "log.h"

struct LpSession {
    uint32_t id;
    PeerConnection *pc;             /* session thread only, once started */
    char *offer;
    atomic_int *force_idr;

    pthread_t thread;
    int thread_started;

    /* Mailbox: everything below is guarded by 'lock'. */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    char *answer;
    int answer_taken;
    int stop;
    int resync;
    LpFrame *queue[LP_SESSION_MAILBOX];
    size_t queue_head;
    size_t queue_count;

    /* Written by the session thread, read by the HTTP thread. */
    atomic_int state;               /* PeerConnectionState */
    atomic_int connected;
    atomic_int finished;
    atomic_int candidates;
    _Atomic uint64_t connect_ms;
    _Atomic uint64_t frames_sent;
    _Atomic uint64_t bytes_sent;
    _Atomic uint64_t frames_dropped;
    _Atomic uint64_t keyframe_requests;
    _Atomic uint64_t multi_slice_frames;
    uint64_t created_ms;
    char end_reason[128];           /* complete before 'finished' is set */

    /* Session thread only. */
    int waiting_idr;
    int slice_warned;
};

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

LpFrame *lp_frame_create(const uint8_t *data, size_t size, int is_idr,
                         uint64_t pts_us)
{
    LpFrame *frame = malloc(sizeof(*frame) + size);

    if (frame == NULL) {
        return NULL;
    }

    atomic_init(&frame->refs, 1);
    frame->is_idr = is_idr;
    frame->pts_us = pts_us;
    frame->size = size;
    memcpy(frame->data, data, size);
    return frame;
}

void lp_frame_ref(LpFrame *frame)
{
    atomic_fetch_add_explicit(&frame->refs, 1, memory_order_relaxed);
}

void lp_frame_unref(LpFrame *frame)
{
    if (frame != NULL &&
        atomic_fetch_sub_explicit(&frame->refs, 1, memory_order_acq_rel) == 1) {
        free(frame);
    }
}

/* ------------------------------------------------------------------ */
/* libpeer callbacks (run on the session thread, or on the HTTP thread  */
/* while the offer is created, before the session thread exists)       */
/* ------------------------------------------------------------------ */

static void on_state_change(PeerConnectionState state, void *user)
{
    LpSession *session = user;

    atomic_store(&session->state, (int) state);
    log_info("libpeer", "session %u: %s", session->id,
             peer_connection_state_to_string(state));
}

static void on_request_keyframe(void *user)
{
    LpSession *session = user;

    atomic_fetch_add(&session->keyframe_requests, 1);
    atomic_store(session->force_idr, 1);
}

/* ------------------------------------------------------------------ */
/* Session thread                                                      */
/* ------------------------------------------------------------------ */

static void finish(LpSession *session, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void finish(LpSession *session, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(session->end_reason, sizeof(session->end_reason), format, args);
    va_end(args);
}

/*
 * Take the queued frames (and the resync flag) in one go, so the lock
 * is never held while libpeer encrypts and sends.
 */
static size_t take_frames(LpSession *session, LpFrame **out, int *resync)
{
    pthread_mutex_lock(&session->lock);

    size_t count = session->queue_count;

    for (size_t i = 0; i < count; i++) {
        out[i] = session->queue[(session->queue_head + i) % LP_SESSION_MAILBOX];
    }

    session->queue_head = 0;
    session->queue_count = 0;
    *resync = session->resync;
    session->resync = 0;

    pthread_mutex_unlock(&session->lock);
    return count;
}

static void drop_frames(LpSession *session)
{
    LpFrame *frames[LP_SESSION_MAILBOX];
    int resync = 0;
    size_t count = take_frames(session, frames, &resync);

    for (size_t i = 0; i < count; i++) {
        lp_frame_unref(frames[i]);
    }
}

static void send_frames(LpSession *session)
{
    LpFrame *frames[LP_SESSION_MAILBOX];
    int resync = 0;
    size_t count = take_frames(session, frames, &resync);

    if (resync && !session->waiting_idr) {
        session->waiting_idr = 1;
        atomic_store(session->force_idr, 1);
    }

    for (size_t i = 0; i < count; i++) {
        LpFrame *frame = frames[i];

        if (session->waiting_idr && !frame->is_idr) {
            atomic_fetch_add(&session->frames_dropped, 1);
            lp_frame_unref(frame);
            continue;
        }

        session->waiting_idr = 0;

        H264AuInfo info;

        h264_au_scan(frame->data, frame->size, &info);

        if (info.slice_count > 1) {
            atomic_fetch_add(&session->multi_slice_frames, 1);

            if (!session->slice_warned) {
                session->slice_warned = 1;
                log_error("libpeer", "session %u: the encoder produced a "
                          "%d-slice picture; libpeer sends each slice as its "
                          "own frame, so the browser will show corruption",
                          session->id, info.slice_count);
            }
        }

        if (peer_connection_send_video(session->pc, frame->data,
                                       frame->size) == 0) {
            atomic_fetch_add(&session->frames_sent, 1);
            atomic_fetch_add(&session->bytes_sent, frame->size);
        } else {
            atomic_fetch_add(&session->frames_dropped, 1);
        }

        lp_frame_unref(frame);
    }
}

static int stop_requested(LpSession *session)
{
    pthread_mutex_lock(&session->lock);
    int stop = session->stop;
    pthread_mutex_unlock(&session->lock);
    return stop;
}

/*
 * Before the answer: sleep on the mailbox condition instead of spinning
 * (peer_connection_loop() returns at once in the NEW state). Returns
 * the answer once it arrived, NULL on timeout or stop.
 */
static char *wait_for_answer(LpSession *session, int timeout_ms)
{
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long) timeout_ms * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;

    pthread_mutex_lock(&session->lock);

    while (session->answer == NULL && !session->stop) {
        if (pthread_cond_timedwait(&session->wake, &session->lock,
                                   &deadline) == ETIMEDOUT) {
            break;
        }
    }

    char *answer = session->answer;

    session->answer = NULL;
    pthread_mutex_unlock(&session->lock);
    return answer;
}

static void *session_thread(void *arg)
{
    LpSession *session = arg;
    uint64_t answered_ms = 0;

    while (!stop_requested(session)) {
        uint64_t now = now_ms();
        int state = atomic_load(&session->state);

        if (state == PEER_CONNECTION_NEW) {
            char *answer = wait_for_answer(session, 50);

            drop_frames(session);

            if (answer != NULL) {
                /*
                 * May block for a few seconds while libpeer resolves
                 * the browser's mDNS host names; this thread is the
                 * only one waiting for it.
                 */
                peer_connection_set_remote_description(session->pc, answer,
                                                       SDP_TYPE_ANSWER);
                free(answer);
                answered_ms = now_ms();
            } else if (now - session->created_ms >
                       LP_SESSION_ANSWER_TIMEOUT_MS) {
                finish(session, "no answer from the browser within %d s",
                       LP_SESSION_ANSWER_TIMEOUT_MS / 1000);
                break;
            }
            continue;
        }

        if (state != PEER_CONNECTION_CHECKING &&
            state != PEER_CONNECTION_CONNECTED) {
            finish(session, "connection %s",
                   peer_connection_state_to_string(
                       (PeerConnectionState) state));
            break;
        }

        /*
         * One ICE/DTLS/RTCP step. Waits at most 1 ms for a packet once
         * connected, which bounds how long a queued frame waits; the
         * DTLS handshake itself runs to completion inside one call.
         */
        peer_connection_loop(session->pc);

        state = atomic_load(&session->state);

        if (state == PEER_CONNECTION_CONNECTED) {
            if (!atomic_load(&session->connected)) {
                atomic_store(&session->connect_ms, now_ms() - answered_ms);
                atomic_store(&session->connected, 1);
                session->waiting_idr = 1;
                atomic_store(session->force_idr, 1);
                log_info("libpeer", "session %u: streaming (ICE + DTLS took "
                         "%llu ms)", session->id,
                         (unsigned long long) atomic_load(&session->connect_ms));
            }

            send_frames(session);
        } else {
            drop_frames(session);

            if (state == PEER_CONNECTION_CHECKING &&
                now_ms() - answered_ms > LP_SESSION_CONNECT_TIMEOUT_MS) {
                finish(session, "ICE/DTLS did not complete within %d s "
                       "(firewall, or no route between the browser's "
                       "candidates and the Pi)",
                       LP_SESSION_CONNECT_TIMEOUT_MS / 1000);
                break;
            }
        }
    }

    if (session->end_reason[0] == 0) {
        finish(session, "closed");
    }

    drop_frames(session);
    peer_connection_destroy(session->pc);
    session->pc = NULL;

    log_info("libpeer", "session %u ended: %s", session->id,
             session->end_reason);
    atomic_store(&session->finished, 1);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

LpSession *lp_session_create(uint32_t id, atomic_int *force_idr,
                             char *error, size_t error_size)
{
    LpSession *session = calloc(1, sizeof(*session));

    if (session == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }

    session->id = id;
    session->force_idr = force_idr;
    session->created_ms = now_ms();
    atomic_init(&session->state, PEER_CONNECTION_NEW);
    pthread_mutex_init(&session->lock, NULL);
    pthread_cond_init(&session->wake, NULL);

    PeerConfiguration config;

    memset(&config, 0, sizeof(config));
    config.video_codec = CODEC_H264;
    config.audio_codec = CODEC_NONE;
    config.datachannel = DATA_CHANNEL_NONE;
    config.on_request_keyframe = on_request_keyframe;
    config.user_data = session;

    session->pc = peer_connection_create(&config);

    if (session->pc == NULL) {
        snprintf(error, error_size, "libpeer: peer_connection_create failed");
        lp_session_destroy(session);
        return NULL;
    }

    /*
     * Required, not optional: libpeer only records a state transition
     * when this callback is registered.
     */
    peer_connection_oniceconnectionstatechange(session->pc, on_state_change);

    const char *offer = peer_connection_create_offer(session->pc);

    if (atomic_load(&session->state) == PEER_CONNECTION_FAILED ||
        offer == NULL) {
        snprintf(error, error_size, "libpeer: DTLS setup failed while "
                 "creating the offer");
        lp_session_destroy(session);
        return NULL;
    }

    if (strstr(offer, "a=candidate:") == NULL) {
        snprintf(error, error_size, "libpeer found no usable network "
                 "interface (no IPv4 address on an interface that is up)");
        lp_session_destroy(session);
        return NULL;
    }

    session->offer = strdup(offer);

    if (session->offer == NULL) {
        snprintf(error, error_size, "out of memory");
        lp_session_destroy(session);
        return NULL;
    }

    if (pthread_create(&session->thread, NULL, session_thread, session) != 0) {
        snprintf(error, error_size, "cannot start the session thread");
        lp_session_destroy(session);
        return NULL;
    }

    session->thread_started = 1;
    return session;
}

uint32_t lp_session_id(const LpSession *session)
{
    return session->id;
}

const char *lp_session_offer(const LpSession *session)
{
    return session->offer;
}

int lp_session_set_answer(LpSession *session, const char *sdp, int candidates)
{
    char *copy = strdup(sdp);

    if (copy == NULL) {
        return -1;
    }

    pthread_mutex_lock(&session->lock);

    if (session->answer_taken || session->stop ||
        atomic_load(&session->finished)) {
        pthread_mutex_unlock(&session->lock);
        free(copy);
        return -1;
    }

    session->answer = copy;
    session->answer_taken = 1;
    atomic_store(&session->candidates, candidates);
    pthread_cond_signal(&session->wake);
    pthread_mutex_unlock(&session->lock);
    return 0;
}

void lp_session_push_frame(LpSession *session, LpFrame *frame)
{
    LpFrame *flushed[LP_SESSION_MAILBOX];
    size_t flushed_count = 0;

    pthread_mutex_lock(&session->lock);

    if (session->stop || atomic_load(&session->finished)) {
        pthread_mutex_unlock(&session->lock);
        return;
    }

    if (session->queue_count == LP_SESSION_MAILBOX) {
        /* The session thread is behind: never let delay build up. */
        for (size_t i = 0; i < session->queue_count; i++) {
            flushed[flushed_count++] =
                session->queue[(session->queue_head + i) % LP_SESSION_MAILBOX];
        }

        session->queue_head = 0;
        session->queue_count = 0;
        session->resync = 1;
    }

    lp_frame_ref(frame);
    session->queue[(session->queue_head + session->queue_count) %
                   LP_SESSION_MAILBOX] = frame;
    session->queue_count++;

    pthread_mutex_unlock(&session->lock);

    if (flushed_count > 0) {
        atomic_fetch_add(&session->frames_dropped, flushed_count);

        for (size_t i = 0; i < flushed_count; i++) {
            lp_frame_unref(flushed[i]);
        }
    }
}

void lp_session_resync(LpSession *session)
{
    pthread_mutex_lock(&session->lock);
    session->resync = 1;
    pthread_mutex_unlock(&session->lock);
}

void lp_session_close(LpSession *session)
{
    pthread_mutex_lock(&session->lock);
    session->stop = 1;
    pthread_cond_signal(&session->wake);
    pthread_mutex_unlock(&session->lock);
}

int lp_session_finished(const LpSession *session)
{
    return atomic_load(&session->finished);
}

void lp_session_get_stats(const LpSession *session, LpSessionStats *out)
{
    memset(out, 0, sizeof(*out));

    out->id = session->id;
    out->finished = atomic_load(&session->finished);
    out->state = out->finished
                     ? "ended"
                     : peer_connection_state_to_string(
                           (PeerConnectionState) atomic_load(&session->state));
    out->connected = atomic_load(&session->connected);
    out->age_ms = now_ms() - session->created_ms;
    out->connect_ms = atomic_load(&session->connect_ms);
    out->frames_sent = atomic_load(&session->frames_sent);
    out->bytes_sent = atomic_load(&session->bytes_sent);
    out->frames_dropped = atomic_load(&session->frames_dropped);
    out->keyframe_requests = atomic_load(&session->keyframe_requests);
    out->multi_slice_frames = atomic_load(&session->multi_slice_frames);
    out->candidates = atomic_load(&session->candidates);

    if (out->finished) {
        snprintf(out->end_reason, sizeof(out->end_reason), "%s",
                 session->end_reason);
    }
}

void lp_session_destroy(LpSession *session)
{
    if (session == NULL) {
        return;
    }

    if (session->thread_started) {
        lp_session_close(session);
        pthread_join(session->thread, NULL);
    } else if (session->pc != NULL) {
        peer_connection_destroy(session->pc);
    }

    for (size_t i = 0; i < session->queue_count; i++) {
        lp_frame_unref(
            session->queue[(session->queue_head + i) % LP_SESSION_MAILBOX]);
    }

    pthread_cond_destroy(&session->wake);
    pthread_mutex_destroy(&session->lock);
    free(session->answer);
    free(session->offer);
    free(session);
}
