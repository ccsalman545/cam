#define _POSIX_C_SOURCE 200809L

/*
 * app_config.c
 *
 * Command line and config file parsing. No dependency beyond libc:
 * the parser is table-free and every accepted key is spelled out in
 * app_config_set_key() so the set of options is auditable in one place.
 */
#include "app_config.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_config_defaults(AppConfig *config)
{
    memset(config, 0, sizeof(*config));

    config->source_kind = SOURCE_V4L2;
    snprintf(config->source_name, sizeof(config->source_name), "v4l2");
    snprintf(config->device, sizeof(config->device), "/dev/video0");
    snprintf(config->listen, sizeof(config->listen), "0.0.0.0");
    snprintf(config->encoder, sizeof(config->encoder), "auto");

    config->width = 640;
    config->height = 480;
    config->fps = 30;
    config->bitrate_kbps = 2500;
    config->keyframe_seconds = 2;

    config->http_port = 8080;
    config->udp_base_port = 50000;

    /*
     * The name is what makes the server reachable without looking up an
     * address, so it is on by default and the operator only has to
     * change it when two cameras share one LAN.
     */
    config->mdns = 1;
    config->mdns_port = 5353;
    snprintf(config->mdns_name, sizeof(config->mdns_name), "camstream");

    config->verbose = 0;
}

static int copy_string(char *destination, size_t capacity, const char *value)
{
    size_t length = strlen(value);

    if (length == 0 || length >= capacity) {
        return -1;
    }

    memcpy(destination, value, length + 1);

    return 0;
}

static int parse_source_kind(const char *value, AppConfig *config)
{
    if (strcmp(value, "v4l2") == 0) {
        config->source_kind = SOURCE_V4L2;
    } else if (strcmp(value, "csi") == 0) {
        config->source_kind = SOURCE_CSI;
    } else if (strcmp(value, "stdin") == 0) {
        config->source_kind = SOURCE_STDIN;
    } else if (strcmp(value, "test") == 0) {
        config->source_kind = SOURCE_TEST;
    } else {
        return -1;
    }

    snprintf(config->source_name, sizeof(config->source_name), "%s", value);

    return 0;
}

static int parse_unsigned(const char *value, uint32_t *out)
{
    if (value[0] == 0) {
        return -1;
    }

    for (const char *p = value; *p != 0; p++) {
        if (!isdigit((unsigned char) *p)) {
            return -1;
        }
    }

    unsigned long parsed = strtoul(value, NULL, 10);

    if (parsed > 0xFFFFFFFFUL) {
        return -1;
    }

    *out = (uint32_t) parsed;

    return 0;
}

/*
 * Apply one key/value pair. Used by both the config file reader and
 * (through app_config_parse) the command line, so the two can never
 * drift apart in accepted spelling or validation.
 *
 * Returns 0 on success, -1 when the key is unknown or the value is
 * malformed.
 */
static int app_config_set_key(AppConfig *config,
                              const char *key,
                              const char *value)
{
    uint32_t number = 0;

    if (strcmp(key, "source") == 0) {
        return parse_source_kind(value, config);
    }

    if (strcmp(key, "device") == 0) {
        return copy_string(config->device, sizeof(config->device), value);
    }

    if (strcmp(key, "rpicam_bin") == 0) {
        return copy_string(config->rpicam_bin, sizeof(config->rpicam_bin),
                           value);
    }

    if (strcmp(key, "listen") == 0) {
        return copy_string(config->listen, sizeof(config->listen), value);
    }

    if (strcmp(key, "encoder") == 0) {
        return copy_string(config->encoder, sizeof(config->encoder), value);
    }

    if (strcmp(key, "mdns_name") == 0) {
        if (value[0] == 0) {
            /*
             * --mdns-name "" is the shortest way to turn the responder
             * off for one run; the config file uses "mdns = off".
             */
            config->mdns = 0;
            return 0;
        }
        return copy_string(config->mdns_name, sizeof(config->mdns_name), value);
    }

    if (strcmp(key, "mdns_port") == 0) {
        if (parse_unsigned(value, &number) != 0 || number == 0 ||
            number > 65535) {
            return -1;
        }
        config->mdns_port = (uint16_t) number;
        return 0;
    }

    if (strcmp(key, "width") == 0) {
        return parse_unsigned(value, &config->width) == 0 ? 0 : -1;
    }

    if (strcmp(key, "height") == 0) {
        return parse_unsigned(value, &config->height) == 0 ? 0 : -1;
    }

    if (strcmp(key, "fps") == 0) {
        return parse_unsigned(value, &config->fps) == 0 ? 0 : -1;
    }

    if (strcmp(key, "bitrate_kbps") == 0) {
        return parse_unsigned(value, &config->bitrate_kbps) == 0 ? 0 : -1;
    }

    if (strcmp(key, "keyframe_seconds") == 0) {
        return parse_unsigned(value, &config->keyframe_seconds) == 0 ? 0 : -1;
    }

    if (strcmp(key, "http_port") == 0) {
        if (parse_unsigned(value, &number) != 0 || number == 0 ||
            number > 65535) {
            return -1;
        }
        config->http_port = (uint16_t) number;
        return 0;
    }

    if (strcmp(key, "udp_port") == 0) {
        if (parse_unsigned(value, &number) != 0 || number == 0 ||
            number > 65535) {
            return -1;
        }
        config->udp_base_port = (uint16_t) number;
        return 0;
    }

    if (strcmp(key, "mdns") == 0) {
        if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
            strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) {
            config->mdns = 1;
            return 0;
        }
        if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
            strcmp(value, "no") == 0 || strcmp(value, "off") == 0) {
            config->mdns = 0;
            return 0;
        }
        return -1;
    }

    if (strcmp(key, "verbose") == 0) {
        if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
            strcmp(value, "yes") == 0) {
            config->verbose = 1;
            return 0;
        }
        if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
            strcmp(value, "no") == 0) {
            config->verbose = 0;
            return 0;
        }
        return -1;
    }

    return -1;
}

static char *trim(char *text)
{
    while (*text != 0 && isspace((unsigned char) *text)) {
        text++;
    }

    char *end = text + strlen(text);

    while (end > text && isspace((unsigned char) end[-1])) {
        end--;
    }

    *end = 0;

    return text;
}

int app_config_load_file(AppConfig *config,
                         const char *path,
                         char *error,
                         size_t error_size)
{
    if (path == NULL || path[0] == 0) {
        return 0;
    }

    FILE *file = fopen(path, "r");

    if (file == NULL) {
        snprintf(error, error_size, "config: cannot open %s", path);
        return -1;
    }

    if (copy_string(config->config_path, sizeof(config->config_path),
                    path) != 0) {
        fclose(file);
        snprintf(error, error_size, "config: path too long: %s", path);
        return -1;
    }

    char line[512];
    unsigned line_number = 0;
    int status = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        char *comment = strchr(line, '#');

        if (comment != NULL) {
            *comment = 0;
        }

        char *text = trim(line);

        if (*text == 0) {
            continue;
        }

        char *equals = strchr(text, '=');

        if (equals == NULL) {
            snprintf(error, error_size,
                     "config: %s:%u: expected 'key = value', got '%s'",
                     path, line_number, text);
            status = -1;
            break;
        }

        *equals = 0;

        char *key = trim(text);
        char *value = trim(equals + 1);

        if (*key == 0 || *value == 0) {
            snprintf(error, error_size,
                     "config: %s:%u: empty key or value", path, line_number);
            status = -1;
            break;
        }

        if (app_config_set_key(config, key, value) != 0) {
            snprintf(error, error_size,
                     "config: %s:%u: unknown key or bad value: %s = %s",
                     path, line_number, key, value);
            status = -1;
            break;
        }
    }

    fclose(file);

    return status;
}

int app_config_parse(AppConfig *config, int argc, char **argv)
{
    int status = 0;

    /*
     * Pass one: --config is honoured before everything else so that
     * command line options override the file regardless of order.
     */
    for (int i = 1; i < argc && status == 0; i++) {
        const char *value = NULL;

        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            value = argv[i + 1];
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            value = argv[i] + 9;
        }

        if (value != NULL) {
            char error[256] = "";

            if (app_config_load_file(config, value, error, sizeof(error)) != 0) {
                fprintf(stderr, "%s\n", error);
                return -1;
            }
        }
    }

    /* Pass two: the remaining options. */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            return 1;
        }

        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("camstream %s\n", APP_VERSION);
            return 1;
        }

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            config->verbose = 1;
            continue;
        }

        if (strcmp(arg, "-t") == 0 || strcmp(arg, "--test") == 0) {
            config->source_kind = SOURCE_TEST;
            snprintf(config->source_name, sizeof(config->source_name), "test");
            continue;
        }

        if (strcmp(arg, "--stdin-yuv420") == 0) {
            config->source_kind = SOURCE_STDIN;
            snprintf(config->source_name, sizeof(config->source_name), "stdin");
            continue;
        }

        /*
         * Options that take a value accept both "--opt value" and
         * "--opt=value".
         */
        char head[64];
        const char *key = NULL;

        if (strncmp(arg, "--", 2) == 0) {
            /* head holds the option name without the leading dashes. */
            const char *name = arg + 2;
            const char *equals = strchr(name, '=');
            size_t name_len = equals != NULL ? (size_t) (equals - name)
                                             : strlen(name);

            if (name_len == 0 || name_len >= sizeof(head)) {
                fprintf(stderr, "unknown option: %s (try --help)\n", arg);
                return -1;
            }

            memcpy(head, name, name_len);
            head[name_len] = 0;
            key = head;
            value = equals != NULL ? equals + 1 : NULL;
        } else if (arg[0] == '-' && arg[1] != 0 && arg[2] == 0) {
            /* Short option, value in the next argv element. */
            head[0] = arg[1];
            head[1] = 0;
            key = head;
            value = NULL;
        } else if (arg[0] == '-' && arg[1] != 0 && arg[2] == '=') {
            /* Short option with an inline value: -s=test. */
            head[0] = arg[1];
            head[1] = 0;
            key = head;
            value = arg + 3;
        } else {
            fprintf(stderr, "unknown option: %s (try --help)\n", arg);
            return -1;
        }

        static const struct {
            const char *short_name;
            const char *long_name;
            const char *config_key;
        } options[] = {
            { "s", "source",           "source" },
            { "d", "device",           "device" },
            { "W", "width",            "width" },
            { "H", "height",           "height" },
            { "F", "fps",              "fps" },
            { "l", "listen",           "listen" },
            { "p", "http-port",        "http_port" },
            { "u", "udp-port",         "udp_port" },
            { "n", "mdns-name",        "mdns_name" },
            { "e", "encoder",          "encoder" },
            { "b", "bitrate",          "bitrate_kbps" },
            { "K", "keyframe",         "keyframe_seconds" },
            { NULL, "rpicam-bin",      "rpicam_bin" },
            { NULL, "config",          NULL }
        };

        int matched = 0;

        for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
            int is_short = options[o].short_name != NULL &&
                           strcmp(key, options[o].short_name) == 0;
            int is_long = strcmp(key, options[o].long_name) == 0;

            if (!is_short && !is_long) {
                continue;
            }

            matched = 1;

            if (value == NULL) {
                if (i + 1 < argc) {
                    value = argv[++i];
                } else {
                    fprintf(stderr, "missing value for %s\n", arg);
                    return -1;
                }
            }

            if (options[o].config_key != NULL &&
                app_config_set_key(config, options[o].config_key, value) != 0) {
                fprintf(stderr, "invalid value for %s: %s\n", arg, value);
                return -1;
            }
            break;
        }

        if (!matched) {
            fprintf(stderr, "unknown option: %s (try --help)\n", arg);
            return -1;
        }
    }

    char error[256] = "";

    if (app_config_validate(config, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error);
        return -1;
    }

    return 0;
}

int app_config_validate(const AppConfig *config, char *error, size_t error_size)
{
    if (config->width < 16 || config->width > 4096 || config->width % 2 != 0) {
        snprintf(error, error_size,
                 "config: width must be an even number between 16 and 4096, "
                 "got %u", config->width);
        return -1;
    }

    if (config->height < 16 || config->height > 4096 ||
        config->height % 2 != 0) {
        snprintf(error, error_size,
                 "config: height must be an even number between 16 and 4096, "
                 "got %u", config->height);
        return -1;
    }

    if (config->fps < 1 || config->fps > 120) {
        snprintf(error, error_size,
                 "config: fps must be between 1 and 120, got %u",
                 config->fps);
        return -1;
    }

    if (config->bitrate_kbps < 100 || config->bitrate_kbps > 100000) {
        snprintf(error, error_size,
                 "config: bitrate_kbps must be between 100 and 100000, got %u",
                 config->bitrate_kbps);
        return -1;
    }

    if (config->keyframe_seconds < 1 || config->keyframe_seconds > 60) {
        snprintf(error, error_size,
                 "config: keyframe_seconds must be between 1 and 60, got %u",
                 config->keyframe_seconds);
        return -1;
    }

    if (config->http_port == 0) {
        snprintf(error, error_size, "config: http_port must not be 0");
        return -1;
    }

    if (config->udp_base_port == 0) {
        snprintf(error, error_size, "config: udp_port must not be 0");
        return -1;
    }

    if (config->listen[0] == 0) {
        snprintf(error, error_size, "config: listen address must not be empty");
        return -1;
    }

    if (config->encoder[0] == 0) {
        snprintf(error, error_size, "config: encoder must not be empty");
        return -1;
    }

    if (config->mdns) {
        size_t length = strlen(config->mdns_name);

        if (length == 0 || length > 63) {
            snprintf(error, error_size,
                     "config: mdns_name must be 1 to 63 characters, got %zu",
                     length);
            return -1;
        }

        if (config->mdns_name[0] == '-' ||
            config->mdns_name[length - 1] == '-') {
            snprintf(error, error_size,
                     "config: mdns_name must not start or end with '-', "
                     "got '%s'", config->mdns_name);
            return -1;
        }

        for (size_t i = 0; i < length; i++) {
            char c = config->mdns_name[i];

            int valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-';

            if (!valid) {
                /* Letters, digits and '-' only: a dot would publish a
                 * different name and a space breaks DNS label rules. */
                snprintf(error, error_size,
                         "config: mdns_name may only contain letters, digits "
                         "and '-', got '%s'", config->mdns_name);
                return -1;
            }
        }
    }

    return 0;
}

void app_config_print_usage(const char *program)
{
    printf(
        "camstream %s\n"
        "\n"
        "Low latency LAN camera streaming server: V4L2 capture, H.264\n"
        "encode, and real WebRTC (ICE-lite, DTLS 1.2, SRTP, RTP) straight\n"
        "to a browser tab.\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Capture:\n"
        "  -s, --source KIND      v4l2 (default), csi, stdin or test\n"
        "  -d, --device PATH      V4L2 capture device (default /dev/video0)\n"
        "  -t, --test             synthetic test pattern, no camera needed\n"
        "      --stdin-yuv420     read raw YUV420 frames from stdin\n"
        "      --rpicam-bin PATH  rpicam-vid or libcamera-vid for -s csi\n"
        "  -W, --width N          capture width (default 640)\n"
        "  -H, --height N         capture height (default 480)\n"
        "  -F, --fps N            capture frame rate (default 30)\n"
        "\n"
        "Encoding:\n"
        "  -e, --encoder MODE     auto (default), hw, hw:/dev/videoN or sw\n"
        "  -b, --bitrate KBPS     target bitrate (default 2500)\n"
        "  -K, --keyframe SEC     keyframe interval in seconds (default 2)\n"
        "\n"
        "Network:\n"
        "  -l, --listen ADDR      HTTP bind address (default 0.0.0.0)\n"
        "  -p, --http-port N      HTTP port for the web UI and WebRTC\n"
        "                         signaling (default 8080)\n"
        "  -u, --udp-port N       first UDP media port (default 50000, one\n"
        "                         port per viewer)\n"
        "  -n, --mdns-name NAME   publish NAME.local over mDNS so a browser\n"
        "                         needs no IP address (default camstream);\n"
        "                         an empty NAME turns the responder off\n"
        "\n"
        "General:\n"
        "      --config PATH      read settings from a config file; command\n"
        "                         line options override the file\n"
        "  -v, --verbose          log DEBUG level messages\n"
        "  -V, --version          print version and exit\n"
        "  -h, --help             this help\n"
        "\n"
        "Config file keys: source, device, rpicam_bin, width, height, fps,\n"
        "encoder, bitrate_kbps, keyframe_seconds, listen, http_port,\n"
        "udp_port, mdns, mdns_name, mdns_port, verbose.\n"
        "\n"
        "Examples:\n"
        "  %s --test --encoder sw                 pipeline check, no camera\n"
        "  %s -d /dev/video0 -W 1280 -H 720       USB webcam\n"
        "  %s -s csi -W 1280 -H 720               Raspberry Pi CSI camera\n"
        "  %s --config /etc/camstream.conf\n"
        "\n",
        APP_VERSION, program, program, program, program, program);
}

void app_config_print_summary(const AppConfig *config)
{
    const char *source_desc = "unknown";

    switch (config->source_kind) {
    case SOURCE_TEST:  source_desc = "test pattern"; break;
    case SOURCE_CSI:   source_desc = "CSI camera (rpicam-vid)"; break;
    case SOURCE_STDIN: source_desc = "stdin (raw YUV420)"; break;
    case SOURCE_V4L2:  source_desc = config->device; break;
    }

    log_info("app", "source     : %s [%s]", source_desc, config->source_name);

    if (config->source_kind == SOURCE_CSI && config->rpicam_bin[0] != 0) {
        log_info("app", "rpicam     : %s", config->rpicam_bin);
    }

    log_info("app", "capture    : %ux%u @ %u fps",
             config->width, config->height, config->fps);
    log_info("app", "encoder    : %s, %u kbps, keyframe every %us",
             config->encoder, config->bitrate_kbps, config->keyframe_seconds);
    log_info("app", "http       : http://%s:%u/ (web UI and signaling)",
             config->listen, config->http_port);
    log_info("app", "udp media  : one port per viewer from %u",
             config->udp_base_port);
    log_info("app", "mdns       : %s",
             config->mdns ? config->mdns_name : "off");

    if (config->config_path[0] != 0) {
        log_info("app", "config file: %s", config->config_path);
    }
}
