/*
 * app_config.h
 *
 * Runtime configuration, shared by every module.
 *
 * Sources of values, in increasing priority:
 *   1. built-in defaults
 *   2. a config file (--config PATH)
 *   3. command line options
 *
 * All strings are fixed size arrays rather than pointers so a whole
 * AppConfig can be copied, compared and re-parsed for
 * POST /api/config/reload without lifetime questions.
 */
#ifndef APP_APP_CONFIG_H
#define APP_APP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define APP_VERSION "2.1.0"

typedef enum {
    SOURCE_V4L2 = 0,        /* V4L2 capture device */
    SOURCE_CSI,             /* Raspberry Pi camera via rpicam-vid pipe */
    SOURCE_STDIN,           /* raw YUV420 on stdin */
    SOURCE_TEST             /* synthetic pattern, no hardware */
} SourceKind;

typedef struct {
    SourceKind source_kind;
    char source_name[8];        /* v4l2 | csi | stdin | test */
    char device[256];           /* V4L2 device path */
    char rpicam_bin[256];       /* empty means autodetect */
    char listen[64];            /* HTTP bind address */
    char encoder[64];           /* auto | hw | hw:/dev/videoN | sw */
    char config_path[256];      /* empty means "no config file" */

    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    uint32_t keyframe_seconds;

    uint16_t http_port;
    uint16_t udp_base_port;     /* first of MAX_SESSIONS consecutive ports */

    int verbose;                /* 1 = log DEBUG */
} AppConfig;

void app_config_defaults(AppConfig *config);

/*
 * Parse argv, applying values on top of whatever the caller already
 * loaded (file, then command line). Returns:
 *    0  parsed
 *    1  --help/--version was handled, caller exits with status 0
 *   -1  error, message already printed
 */
int app_config_parse(AppConfig *config, int argc, char **argv);

/*
 * Read a `key = value` config file. Unknown keys, malformed lines and
 * out of range values are errors: a silently ignored setting is worse
 * than a refused start. Returns 0 on success, -1 on failure with a
 * message in 'error' naming the file, the line and the problem.
 */
int app_config_load_file(AppConfig *config,
                         const char *path,
                         char *error,
                         size_t error_size);

/* Range and consistency checks. Returns 0 or -1 with a message. */
int app_config_validate(const AppConfig *config,
                        char *error,
                        size_t error_size);

void app_config_print_usage(const char *program);

void app_config_print_summary(const AppConfig *config);

#endif
