/*
 * app_config.h
 *
 * Command line configuration shared by every module.
 */
#ifndef APP_APP_CONFIG_H
#define APP_APP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define APP_VERSION "2.0.0"

typedef enum {
    SOURCE_V4L2 = 0,
    SOURCE_CSI,
    SOURCE_STDIN,
    SOURCE_TEST
} SourceKind;

typedef struct {
    SourceKind source_kind;     /* SOURCE_V4L2 | SOURCE_CSI | SOURCE_STDIN | SOURCE_TEST */
    const char *source_name;    /* "v4l2", "csi", "stdin", "test" */
    const char *rpicam_bin;     /* optional path to rpicam-vid / libcamera-vid */
    const char *device;         /* V4L2 device path */
    int use_test_source;        /* synthetic pattern instead of a camera */

    uint32_t width;
    uint32_t height;
    uint32_t fps;

    const char *listen;         /* HTTP listen address, default 0.0.0.0 */
    uint16_t http_port;
    uint16_t udp_base_port;     /* first UDP port for RTC sessions */

    uint32_t bitrate_kbps;
    uint32_t keyframe_seconds;

    const char *webrtc_backend; /* native | libpeer | janus */

    /* Janus transport (used by the camstream-janus build). */
    const char *janus_host;     /* Janus gateway address */
    uint16_t janus_rtp_port;    /* Janus video RTP port (videoport) */
    uint16_t janus_rtcp_port;   /* Janus video RTCP port (videortcpport) */
    uint16_t janus_rtcp_listen; /* local port for RTCP feedback from Janus */

    const char *encoder;        /* auto | hw | hw:<path> | sw */

    int verbose;
} AppConfig;

/*
 * Fill defaults.
 */
void app_config_defaults(AppConfig *config);

/*
 * Parse argv. Returns 0 on success, 1 when the caller should
 * exit with status 0 ( --help / --version ), -1 on error.
 */
int app_config_parse(AppConfig *config, int argc, char **argv);

void app_config_print_usage(const char *program);

void app_config_print_summary(const AppConfig *config);

#endif
