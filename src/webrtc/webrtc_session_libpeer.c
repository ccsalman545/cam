#define _POSIX_C_SOURCE 200809L
/*
 * webrtc_session_libpeer.c
 *
 * libpeer-based implementation of the RtcSession API.
 * One browser viewer = one PeerConnection.
 *
 * This file is compiled when USE_LIBPEER=1. It replaces the
 * native ICE-lite/DTLS-SRTP/RTP implementation with libpeer,
 * which uses mbedTLS + libsrtp + usrsctp internally.
 *
 * Life cycle mirrors the native version:
 *   NEW -> CHECKING -> CONNECTED/COMPLETED -> CLOSED
 * but the underlying PeerConnection handles ICE, DTLS, SRTP,
 * RTP packetization.
 *
 * Signaling: browser sends offer SDP via POST /rtc/offer,
 * we call peer_connection_set_remote_description(offer),
 * then peer_connection_create_answer() and return answer.
 *
 * Media: encoder thread pushes Annex-B H.264 AUs via
 * rtc_session_send_access_unit() -> peer_connection_send_video()
 *
 * The main thread must call peer_connection_loop() regularly
 * via rtc_session_tick().
 */

#include "webrtc_session.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_LIBPEER

/* Try to include libpeer headers from various possible locations */
#if __has_include("peer.h")
#include "peer.h"
#include "peer_connection.h"
#elif __has_include("peer/peer.h")
#include "peer/peer.h"
#include "peer/peer_connection.h"
#else
/* Fallback: forward declare to allow compilation without libpeer headers
 * (stub mode). The real build will have headers in build/libpeer/dist/include */
typedef struct PeerConnection PeerConnection;
typedef enum {
  SDP_TYPE_OFFER = 0,
  SDP_TYPE_ANSWER,
} SdpType;
typedef enum {
  PEER_CONNECTION_CLOSED = 0,
  PEER_CONNECTION_NEW,
  PEER_CONNECTION_CHECKING,
  PEER_CONNECTION_CONNECTED,
  PEER_CONNECTION_COMPLETED,
  PEER_CONNECTION_FAILED,
  PEER_CONNECTION_DISCONNECTED,
} PeerConnectionState;
typedef enum {
  CODEC_NONE = 0,
  CODEC_H264,
  CODEC_VP8,
  CODEC_MJPEG,
  CODEC_OPUS,
  CODEC_PCMA,
  CODEC_PCMU,
} MediaCodec;
typedef enum {
  DATA_CHANNEL_NONE = 0,
  DATA_CHANNEL_STRING,
  DATA_CHANNEL_BINARY,
} DataChannelType;
typedef struct {
  const char* urls;
  const char* username;
  const char* credential;
} IceServer;
typedef struct {
  IceServer ice_servers[5];
  MediaCodec audio_codec;
  MediaCodec video_codec;
  DataChannelType datachannel;
  void (*onaudiotrack)(uint8_t* data, size_t size, void* userdata);
  void (*onvideotrack)(uint8_t* data, size_t size, void* userdata);
  void (*on_request_keyframe)(void* userdata);
  void* user_data;
} PeerConfiguration;
static const char* peer_connection_state_to_string(PeerConnectionState s){(void)s;return "unknown";}
static PeerConnectionState peer_connection_get_state(PeerConnection* pc){(void)pc;return PEER_CONNECTION_NEW;}
static PeerConnection* peer_connection_create(PeerConfiguration* c){(void)c;return NULL;}
static void peer_connection_destroy(PeerConnection* pc){(void)pc;}
static void peer_connection_close(PeerConnection* pc){(void)pc;}
static int peer_connection_loop(PeerConnection* pc){(void)pc;return 0;}
static int peer_connection_send_video(PeerConnection* pc, const uint8_t* p, size_t b){(void)pc;(void)p;(void)b;return -1;}
static void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType t){(void)pc;(void)sdp;(void)t;}
static const char* peer_connection_create_answer(PeerConnection* pc){(void)pc;return NULL;}
static void peer_connection_oniceconnectionstatechange(PeerConnection* pc, void (*cb)(PeerConnectionState, void*)){ (void)pc;(void)cb; }
static void peer_connection_onicecandidate(PeerConnection* pc, void (*cb)(char*, void*)){ (void)pc;(void)cb; }
static int peer_connection_add_ice_candidate(PeerConnection* pc, char* cand){(void)pc;(void)cand;return 0;}
static int peer_init(){return 0;}
static void peer_deinit(){}
#endif

#define SESSION_IDLE_TIMEOUT_MS 15000
#define SESSION_DTLS_WATCHDOG_MS 30000
#define IDR_MIN_INTERVAL_MS 400

struct RtcSession {
    RtcSessionConfig config;
    RtcSessionState state;
    PeerConnection* pc;

    char local_ufrag[16]; /* kept for logging compatibility */
    char local_pwd[44];

    uint64_t created_ms;
    uint64_t last_rx_ms;
    uint64_t last_idr_ms;
    uint64_t last_loop_ms;
    int idr_requested;
    int streaming_announced;
    unsigned datagrams_logged;

    RtcSessionStats stats;

    char* remote_sdp_copy;
    char* local_sdp_copy;

    int closed;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void set_state(RtcSession* session, RtcSessionState state)
{
    if (session->state == state) return;
    session->state = state;
    printf("rtc %08x [libpeer]: state -> %s\n", session->config.id, rtc_session_state_name(session));
}

/* Callbacks from libpeer */

static void on_ice_state_change(PeerConnectionState pc_state, void* userdata)
{
    RtcSession* session = (RtcSession*)userdata;
    if (!session) return;

    printf("rtc %08x [libpeer]: ice state %s (%d)\n",
           session->config.id,
           peer_connection_state_to_string(pc_state),
           (int)pc_state);

    switch (pc_state) {
    case PEER_CONNECTION_NEW:
        set_state(session, RTC_NEW);
        break;
    case PEER_CONNECTION_CHECKING:
        set_state(session, RTC_ICE);
        break;
    case PEER_CONNECTION_CONNECTED:
        set_state(session, RTC_STREAMING);
        session->last_rx_ms = now_ms();
        if (!session->streaming_announced) {
            /* trigger IDR for fresh viewer */
            rtc_session_request_idr(session);
        }
        break;
    case PEER_CONNECTION_FAILED:
    case PEER_CONNECTION_CLOSED:
    case PEER_CONNECTION_DISCONNECTED:
        printf("rtc %08x [libpeer]: connection failed/closed\n", session->config.id);
        rtc_session_close(session);
        break;
    default:
        break;
    }
}

static void on_ice_candidate(char* sdp_text, void* userdata)
{
    RtcSession* session = (RtcSession*)userdata;
    if (!session) return;
    if (sdp_text) {
        printf("rtc %08x [libpeer]: local candidate %s\n", session->config.id, sdp_text);
        session->stats.stun_ok++;
    }
}

static void on_request_keyframe(void* userdata)
{
    RtcSession* session = (RtcSession*)userdata;
    if (!session) return;
    printf("rtc %08x [libpeer]: keyframe requested (PLI/FIR)\n", session->config.id);
    session->stats.pli_received++;
    rtc_session_request_idr(session);
}

/* Global init handled in libpeer_global.c */

/* API */

int rtc_session_create(const RtcSessionConfig* config,
                       RtcSession** session_out,
                       char* answer_sdp,
                       size_t answer_capacity,
                       size_t* answer_length)
{
    *session_out = NULL;

    if (!config || !config->remote_sdp) {
        fprintf(stderr, "rtc: libpeer create failed - no remote SDP\n");
        return -1;
    }

    RtcSession* session = calloc(1, sizeof(*session));
    if (!session) return -1;

    session->config = *config;
    session->state = RTC_NEW;
    session->created_ms = now_ms();
    session->last_rx_ms = session->created_ms;
    session->remote_sdp_copy = strdup(config->remote_sdp);
    if (!session->remote_sdp_copy) {
        free(session);
        return -1;
    }

    /* PeerConfiguration: recvonly video, H264 */
    PeerConfiguration pc_config;
    memset(&pc_config, 0, sizeof(pc_config));
    pc_config.video_codec = CODEC_H264;
    pc_config.audio_codec = CODEC_NONE;
    pc_config.datachannel = DATA_CHANNEL_NONE;
    pc_config.on_request_keyframe = on_request_keyframe;
    pc_config.user_data = session;
    /* No STUN/TURN for direct LAN - host candidates only */
    pc_config.ice_servers[0].urls = NULL;

    session->pc = peer_connection_create(&pc_config);
    if (!session->pc) {
        fprintf(stderr, "rtc %08x [libpeer]: peer_connection_create failed (libpeer not built?)\n", config->id);
        free(session->remote_sdp_copy);
        free(session);
        return -1;
    }

    peer_connection_oniceconnectionstatechange(session->pc, on_ice_state_change);
    peer_connection_onicecandidate(session->pc, on_ice_candidate);

    /* Set remote offer */
    peer_connection_set_remote_description(session->pc, session->remote_sdp_copy, SDP_TYPE_OFFER);

    /* Create answer */
    const char* answer = peer_connection_create_answer(session->pc);
    if (!answer) {
        fprintf(stderr, "rtc %08x [libpeer]: create_answer failed\n", config->id);
        peer_connection_destroy(session->pc);
        free(session->remote_sdp_copy);
        free(session);
        return -1;
    }

    session->local_sdp_copy = strdup(answer);
    if (!session->local_sdp_copy) {
        peer_connection_destroy(session->pc);
        free(session->remote_sdp_copy);
        free(session);
        return -1;
    }

    size_t len = strlen(answer);
    if (len + 1 > answer_capacity) {
        fprintf(stderr, "rtc %08x [libpeer]: answer overflow %zu > %zu\n", config->id, len, answer_capacity);
        peer_connection_destroy(session->pc);
        free(session->remote_sdp_copy);
        free(session->local_sdp_copy);
        free(session);
        return -1;
    }

    memcpy(answer_sdp, answer, len + 1);
    *answer_length = len;

    /* Clear dangling extra IP pointers that were on caller's stack */
    session->config.extra_ips = NULL;
    session->config.extra_ip_count = 0;

    *session_out = session;

    printf("rtc %08x [libpeer]: created, answer %zu bytes, remote offer %zu bytes\n",
           config->id, len, strlen(session->remote_sdp_copy));

    /* Immediately go to ICE checking - libpeer will gather host candidates */
    set_state(session, RTC_ICE);

    return 0;
}

int rtc_session_fd(const RtcSession* session)
{
    (void)session;
    /* libpeer manages its own UDP socket internally */
    return -1;
}

void rtc_session_on_udp(RtcSession* session,
                        uint8_t* buffer,
                        size_t length,
                        const struct sockaddr_storage* source)
{
    (void)session;
    (void)buffer;
    (void)length;
    (void)source;
    /* No-op: libpeer handles UDP internally via peer_connection_loop */
}

void rtc_session_tick(RtcSession* session, uint64_t now)
{
    if (!session || session->state == RTC_CLOSED) return;

    /* libpeer main loop - must be called frequently */
    if (session->pc) {
        peer_connection_loop(session->pc);
    }

    /* Idle timeout */
    if (now - session->last_rx_ms > SESSION_IDLE_TIMEOUT_MS) {
        /* Only timeout if never connected */
        if (session->state == RTC_NEW || session->state == RTC_ICE) {
            printf("rtc %08x [libpeer]: idle timeout (no ICE)\n", session->config.id);
            rtc_session_close(session);
            return;
        }
    }

    /* DTLS watchdog */
    if (session->state == RTC_ICE && now - session->created_ms > SESSION_DTLS_WATCHDOG_MS) {
        printf("rtc %08x [libpeer]: DTLS never started\n", session->config.id);
        rtc_session_close(session);
        return;
    }
}

int rtc_session_send_access_unit(RtcSession* session,
                                 const uint8_t* access_unit,
                                 size_t length,
                                 uint64_t pts_us,
                                 int is_idr)
{
    (void)pts_us;
    if (!session || !session->pc) return 0;
    if (session->state != RTC_STREAMING) return 0;

    if (is_idr && session->idr_requested) {
        session->idr_requested = 0;
    }

    /* libpeer expects raw H.264 Annex-B frame */
    int ret = peer_connection_send_video(session->pc, access_unit, length);
    if (ret == 0) {
        session->stats.packets_sent++;
        session->stats.bytes_sent += length;
        if (!session->streaming_announced) {
            session->streaming_announced = 1;
            printf("rtc %08x [libpeer]: streaming video (%zu bytes %s)\n",
                   session->config.id, length, is_idr ? "IDR" : "P");
        }
        return 1;
    } else {
        /* Send failed - maybe not connected yet */
        return 0;
    }
}

void rtc_session_request_idr(RtcSession* session)
{
    if (!session || !session->config.on_idr_request) return;

    uint64_t now = now_ms();
    if (session->last_idr_ms != 0 && now - session->last_idr_ms < IDR_MIN_INTERVAL_MS) {
        return;
    }
    session->last_idr_ms = now;
    session->idr_requested = 1;
    session->config.on_idr_request(session->config.server);
}

RtcSessionState rtc_session_state(const RtcSession* session)
{
    return session ? session->state : RTC_CLOSED;
}

const char* rtc_session_state_name(const RtcSession* session)
{
    if (!session) return "closed";
    switch (session->state) {
    case RTC_NEW: return "new";
    case RTC_ICE: return "ice";
    case RTC_DTLS: return "dtls";
    case RTC_STREAMING: return "streaming";
    default: return "closed";
    }
}

void rtc_session_get_stats(const RtcSession* session, RtcSessionStats* out)
{
    if (session && out) *out = session->stats;
}

uint32_t rtc_session_id(const RtcSession* session)
{
    return session ? session->config.id : 0;
}

void rtc_session_close(RtcSession* session)
{
    if (!session || session->state == RTC_CLOSED) return;

    if (session->pc) {
        peer_connection_close(session->pc);
    }

    set_state(session, RTC_CLOSED);

    if (session->config.on_closed) {
        session->config.on_closed(session->config.server, session);
    }
}

void rtc_session_destroy(RtcSession* session)
{
    if (!session) return;

    if (session->state != RTC_CLOSED) {
        rtc_session_close(session);
    }

    if (session->pc) {
        peer_connection_destroy(session->pc);
        session->pc = NULL;
    }

    free(session->remote_sdp_copy);
    free(session->local_sdp_copy);
    free(session);
}

int rtc_session_add_ice_candidate(RtcSession* session, const char* candidate)
{
    if (!session || !session->pc || !candidate || !candidate[0]) return -1;
    char cand_buf[512];
    strncpy(cand_buf, candidate, sizeof(cand_buf) - 1);
    cand_buf[sizeof(cand_buf) - 1] = '\0';
    return peer_connection_add_ice_candidate(session->pc, cand_buf);
}

int rtc_session_dtls_timeout_ms(const RtcSession* session)
{
    (void)session;
    /* libpeer handles retransmission internally */
    return -1;
}

#else /* !USE_LIBPEER */

/* Stub that forwards to native implementation when USE_LIBPEER not defined.
 * This file is not compiled in native mode, but we provide a dummy to
 * satisfy linker if accidentally included.
 */

#include <stdio.h>
int rtc_session_create(const RtcSessionConfig *config, RtcSession **out, char *ans, size_t cap, size_t *len){(void)config;(void)out;(void)ans;(void)cap;(void)len; fprintf(stderr,"libpeer disabled\n"); return -1;}
int rtc_session_fd(const RtcSession *s){(void)s;return -1;}
void rtc_session_on_udp(RtcSession *s, uint8_t *b, size_t l, const struct sockaddr_storage *src){(void)s;(void)b;(void)l;(void)src;}
void rtc_session_tick(RtcSession *s, uint64_t n){(void)s;(void)n;}
int rtc_session_send_access_unit(RtcSession *s, const uint8_t *a, size_t l, uint64_t p, int i){(void)s;(void)a;(void)l;(void)p;(void)i;return 0;}
void rtc_session_request_idr(RtcSession *s){(void)s;}
int rtc_session_add_ice_candidate(RtcSession *s, const char *c){(void)s;(void)c;return 0;}
RtcSessionState rtc_session_state(const RtcSession *s){(void)s;return RTC_CLOSED;}
const char *rtc_session_state_name(const RtcSession *s){(void)s;return "closed";}
void rtc_session_get_stats(const RtcSession *s, RtcSessionStats *o){(void)s;(void)o;}
uint32_t rtc_session_id(const RtcSession *s){(void)s;return 0;}
void rtc_session_close(RtcSession *s){(void)s;}
void rtc_session_destroy(RtcSession *s){(void)s;}
int rtc_session_dtls_timeout_ms(const RtcSession *s){(void)s;return -1;}

#endif /* USE_LIBPEER */
