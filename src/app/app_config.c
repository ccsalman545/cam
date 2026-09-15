#define _POSIX_C_SOURCE 200809L

/*
 * app_config.c
 *
 * Dependency free command line parsing (short and long
 * options, "=" and separate value forms).
 */
#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_config_defaults(AppConfig *config)
{
    config->device = "/dev/video0";
    config->use_test_source = 0;

    config->width = 640;
    config->height = 480;
    config->fps = 30;

    config->listen = "0.0.0.0";
#ifdef USE_LIBPEER
    config->http_port = 8000;
#else
    config->http_port = 8080;
#endif
    config->udp_base_port = 50000;

#ifdef USE_JANUS_TRANSPORT
    config->webrtc_backend = "janus";
    config->janus_host = "127.0.0.1";
    config->janus_rtp_port = 5004;
    config->janus_rtcp_port = 5005;
    config->janus_rtcp_listen = 5006;
#elif defined(USE_LIBPEER)
    config->webrtc_backend = "libpeer";
#else
    config->webrtc_backend = "native";
#endif

    config->bitrate_kbps = 2500;
    config->keyframe_seconds = 2;

    config->encoder = "auto";

    config->verbose = 0;
}

static int match_opt(const char *arg,
                     const char *short_name,
                     const char *long_name,
                     const char **value,
                     int argc,
                     char **argv,
                     int *index)
{
    /*
     * Accepts: -x value, -x=value, --name value, --name=value
     */
    const char *eq = strchr(arg, '=');
    size_t head_len = eq != NULL ? (size_t) (eq - arg) : strlen(arg);

    char head[64];

    if (head_len >= sizeof(head)) {
        return 0;
    }

    memcpy(head, arg, head_len);
    head[head_len] = 0;

    int matched_short = short_name != NULL && strcmp(head, short_name) == 0;
    int matched_long = long_name != NULL && strcmp(head, long_name) == 0;

    if (!matched_short && !matched_long) {
        return 0;
    }

    if (eq != NULL) {
        *value = eq + 1;
        return 1;
    }

    if (*index + 1 < argc) {
        *value = argv[++(*index)];
        return 1;
    }

    fprintf(stderr, "missing value for %s\n", head);
    exit(2);
}

static long parse_long(const char *value, const char *name)
{
    char *end = NULL;

    long result = strtol(value, &end, 10);

    if (end == NULL || *end != 0 || result < 0) {
        fprintf(stderr, "invalid number for %s: %s\n", name, value);
        exit(2);
    }

    return result;
}

int app_config_parse(AppConfig *config, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            return 1;
        }

        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("camstream %s\n", APP_VERSION);
            exit(0);
        }

        if (strcmp(arg, "-t") == 0 || strcmp(arg, "--test") == 0) {
            config->use_test_source = 1;
            continue;
        }

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            config->verbose = 1;
            continue;
        }

        if (match_opt(arg, "-d", "--device", &value, argc, argv, &i)) {
            config->device = value;
            continue;
        }

        if (match_opt(arg, "-W", "--width", &value, argc, argv, &i)) {
            config->width = (uint32_t) parse_long(value, "width");
            continue;
        }

        if (match_opt(arg, "-H", "--height", &value, argc, argv, &i)) {
            config->height = (uint32_t) parse_long(value, "height");
            continue;
        }

        if (match_opt(arg, "-F", "--fps", &value, argc, argv, &i)) {
            config->fps = (uint32_t) parse_long(value, "fps");
            continue;
        }

        if (match_opt(arg, "-l", "--listen", &value, argc, argv, &i)) {
            config->listen = value;
            continue;
        }

        if (match_opt(arg, "-p", "--http-port", &value, argc, argv, &i)) {
            config->http_port = (uint16_t) parse_long(value, "http-port");
            continue;
        }

        if (match_opt(arg, "-u", "--udp-port", &value, argc, argv, &i)) {
            config->udp_base_port = (uint16_t) parse_long(value, "udp-port");
            continue;
        }

        if (match_opt(arg, NULL, "--webrtc", &value, argc, argv, &i)) {
            if (strcmp(value, "native") != 0 &&
                strcmp(value, "libpeer") != 0 &&
                strcmp(value, "janus") != 0) {
                fprintf(stderr,
                        "invalid --webrtc '%s' (native, libpeer or janus)\n",
                        value);
                return -1;
            }
            config->webrtc_backend = value;
            continue;
        }

        if (match_opt(arg, NULL, "--janus-host", &value, argc, argv, &i)) {
            config->janus_host = value;
            continue;
        }

        if (match_opt(arg, NULL, "--janus-rtp-port", &value, argc, argv, &i)) {
            config->janus_rtp_port = (uint16_t) parse_long(value, "janus-rtp-port");
            continue;
        }

        if (match_opt(arg, NULL, "--janus-rtcp-port", &value, argc, argv, &i)) {
            config->janus_rtcp_port = (uint16_t) parse_long(value, "janus-rtcp-port");
            continue;
        }

        if (match_opt(arg, NULL, "--janus-rtcp-listen", &value, argc, argv, &i)) {
            config->janus_rtcp_listen = (uint16_t) parse_long(value, "janus-rtcp-listen");
            continue;
        }

        if (match_opt(arg, "-b", "--bitrate", &value, argc, argv, &i)) {
            config->bitrate_kbps = (uint32_t) parse_long(value, "bitrate");
            continue;
        }

        if (match_opt(arg, "-K", "--keyframe", &value, argc, argv, &i)) {
            config->keyframe_seconds = (uint32_t) parse_long(value, "keyframe");
            continue;
        }

        if (match_opt(arg, "-e", "--encoder", &value, argc, argv, &i)) {
            config->encoder = value;
            continue;
        }

        fprintf(stderr, "unknown option: %s (try --help)\n", arg);
        return -1;
    }

    if (config->width < 2 || config->width > 4096 ||
        config->height < 2 || config->height > 4096 ||
        config->fps == 0 || config->fps > 120 ||
        config->http_port == 0 || config->udp_base_port == 0 ||
        config->bitrate_kbps == 0 || config->keyframe_seconds == 0) {
        fprintf(stderr, "invalid configuration: dimensions, fps, ports, bitrate "
                        "and keyframe interval must be positive and in range\n");
        return -1;
    }

    /*
     * Each build is compiled for exactly one transport; refuse to
     * pretend to run a transport that was not linked in.
     */
#if defined(USE_JANUS_TRANSPORT)
    if (strcmp(config->webrtc_backend, "janus") != 0) {
        fprintf(stderr,
                "this binary is the Janus transport build (camstream-janus). "
                "Use camstream for native WebRTC or camstream-libpeer for "
                "libpeer, with --webrtc native or --webrtc libpeer.\n");
        return -1;
    }

    if (config->janus_rtp_port == 0 || config->janus_rtcp_port == 0 ||
        config->janus_rtcp_listen == 0) {
        fprintf(stderr, "invalid Janus ports: --janus-rtp-port, "
                        "--janus-rtcp-port and --janus-rtcp-listen must be "
                        "nonzero\n");
        return -1;
    }
#elif defined(USE_LIBPEER)
    if (strcmp(config->webrtc_backend, "libpeer") != 0) {
        fprintf(stderr,
                "this binary is the libpeer build (camstream-libpeer). "
                "Use camstream for native WebRTC or camstream-janus for the "
                "Janus transport.\n");
        return -1;
    }
#else
    if (strcmp(config->webrtc_backend, "native") != 0) {
        fprintf(stderr,
                "this binary is the native WebRTC build (camstream). "
                "The %s transport is a separate binary: %s\n",
                config->webrtc_backend,
                strcmp(config->webrtc_backend, "libpeer") == 0 ?
                    "make libpeer && make camstream-libpeer" :
                    "make camstream-janus");
        return -1;
    }
#endif

    return 0;
}

void app_config_print_usage(const char *program)
{
    printf(
        "camstream %s\n"
        "\n"
        "Low latency camera to browser streaming server with real WebRTC\n"
        "media transport (ICE-lite, DTLS 1.2, SRTP, RTP H.264).\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Source:\n"
        "  -d, --device PATH     V4L2 device (default /dev/video0)\n"
        "  -t, --test            use the synthetic test pattern source\n"
        "  -W, --width N         capture width (default 640)\n"
        "  -H, --height N        capture height (default 480)\n"
        "  -F, --fps N           frames per second (default 30)\n"
        "\n"
        "Network:\n"
        "  -l, --listen ADDR     HTTP listen address (default 0.0.0.0)\n"
        "  -p, --http-port N     HTTP port for the web UI and\n"
        "                        WebRTC signaling (default 8080)\n"
        "  -u, --udp-port N      base UDP port for media sessions\n"
        "                        (default 50000, one port per viewer)\n"
        "\n"
        "WebRTC transport:\n"
        "  --webrtc MODE         native (default), libpeer or janus.\n"
        "                        Each build is compiled for exactly one:\n"
        "                        camstream (native), camstream-libpeer\n"
        "                        and camstream-janus respectively.\n"
#ifdef USE_JANUS_TRANSPORT
        "\n"
        "Janus transport (camstream-janus build):\n"
        "  --janus-host ADDR     Janus gateway address (default 127.0.0.1)\n"
        "  --janus-rtp-port N    Janus video RTP port, videoport\n"
        "                        (default 5004)\n"
        "  --janus-rtcp-port N   Janus video RTCP port, videortcpport,\n"
        "                        receives sender reports (default 5005)\n"
        "  --janus-rtcp-listen N local port for PLI/FIR feedback from\n"
        "                        Janus (default 5006)\n"
#endif
        "\n"
        "Encoding:\n"
        "  -e, --encoder MODE    auto (default), hw, hw:/dev/videoNN, sw\n"
        "  -b, --bitrate KBPS    target bitrate (default 2500)\n"
        "  -K, --keyframe SEC    keyframe interval in seconds (default 2)\n"
        "\n"
        "Misc:\n"
        "  -v, --verbose         verbose logging\n"
        "  -V, --version         print version and exit\n"
        "  -h, --help            this help\n"
        "\n"
        "Examples:\n"
        "  %s -t                          run with the test pattern\n"
        "  %s -d /dev/video0 -W 1280 -H 720 -b 4000\n"
        "  %s -e hw:/dev/video11 -p 8080 -u 50000\n"
#ifdef USE_JANUS_TRANSPORT
        "  %s -t -e hw --janus-host 127.0.0.1 "
        "--janus-rtp-port 5004\n"
        "                                         H.264 -> RTP -> Janus -> "
        "WebRTC (install config/janus/*.jcfg into /etc/janus first)\n"
#endif
        "\n",
        APP_VERSION, program, program, program, program
#ifdef USE_JANUS_TRANSPORT
        , program
#endif
        );
}

void app_config_print_summary(const AppConfig *config)
{
    printf("source        : %s\n",
           config->use_test_source ? "test pattern" : config->device);
    printf("resolution    : %ux%u @ %u fps\n",
           config->width, config->height, config->fps);
    printf("encoder       : %s, %u kbps, keyframe every %us\n",
           config->encoder, config->bitrate_kbps, config->keyframe_seconds);
    printf("transport     : %s\n", config->webrtc_backend);
    printf("http          : http://%s:%u/  (web UI%s)\n",
           config->listen, config->http_port,
#ifdef USE_JANUS_TRANSPORT
           ""
#else
           " + WebRTC signaling"
#endif
           );
#ifdef USE_JANUS_TRANSPORT
    printf("janus rtp     : %s:%u (RTP H.264 video -> Janus)\n",
           config->janus_host, config->janus_rtp_port);
    printf("janus rtcp    : %s:%u (sender reports -> Janus)\n",
           config->janus_host, config->janus_rtcp_port);
    printf("rtcp listen   : :%u (PLI/FIR keyframe requests <- Janus)\n",
           config->janus_rtcp_listen);
    printf("signaling     : handled by Janus (see config/janus/janus.jcfg)\n");
#else
    printf("udp media     : ports from %u\n", config->udp_base_port);
#endif
}
