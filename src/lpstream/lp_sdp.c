#define _POSIX_C_SOURCE 200809L

/*
 * lp_sdp.c
 *
 * Browser SDP answer -> libpeer-safe SDP. See lp_sdp.h for the libpeer
 * parser limits every rule here protects.
 */
#include "lp_sdp.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libpeer field sizes (third_party/libpeer/src/ice.h, address.h). */
#define LP_FOUNDATION_MAX 32
#define LP_ADDRESS_MAX 45           /* ADDRSTRLEN (46) minus the NUL */
#define LP_UFRAG_MAX 64             /* libpeer allows 256; browsers use 4-32 */
#define LP_PWD_MAX 128
#define LP_FINGERPRINT_PREFIX "a=fingerprint:sha-256 "
#define LP_FINGERPRINT_LENGTH 95    /* 32 bytes as "AB:" pairs */

typedef struct {
    char *buffer;
    size_t size;
    size_t length;
    int overflow;
} Out;

static void out_line(Out *out, const char *line, size_t length)
{
    if (out->overflow || out->length + length + 3 > out->size) {
        out->overflow = 1;
        return;
    }

    memcpy(out->buffer + out->length, line, length);
    out->length += length;
    out->buffer[out->length++] = '\r';
    out->buffer[out->length++] = '\n';
    out->buffer[out->length] = 0;
}

static int fail(char *error, size_t error_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static int fail(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
    return -1;
}

static int starts_with(const char *line, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);

    return length >= prefix_length && memcmp(line, prefix, prefix_length) == 0;
}

static int all_digits(const char *text, size_t max_digits)
{
    size_t n = strlen(text);

    if (n == 0 || n > max_digits) {
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char) text[i])) {
            return 0;
        }
    }

    return 1;
}

static int ice_char(int c)
{
    /* RFC 8445 ice-char: ALPHA / DIGIT / "+" / "/" */
    return isalnum(c) || c == '+' || c == '/';
}

static int valid_ice_token(const char *value, size_t length, size_t max)
{
    if (length == 0 || length > max) {
        return 0;
    }

    for (size_t i = 0; i < length; i++) {
        if (!ice_char((unsigned char) value[i])) {
            return 0;
        }
    }

    return 1;
}

static int valid_mdns_name(const char *name)
{
    size_t n = strlen(name);
    static const char suffix[] = ".local";

    if (n <= sizeof(suffix) - 1 || n > LP_ADDRESS_MAX ||
        strcmp(name + n - (sizeof(suffix) - 1), suffix) != 0) {
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char) name[i];

        if (!isalnum(c) && c != '-' && c != '.') {
            return 0;
        }
    }

    return 1;
}

/*
 * Returns 1 when libpeer can use the candidate, 0 when it has to be
 * dropped. *is_mdns tells whether the address is a ".local" name.
 */
static int usable_candidate(const char *line, size_t length, int *is_mdns)
{
    char copy[LP_SDP_MAX_LINE + 1];
    char *fields[8];
    int count = 0;

    memcpy(copy, line, length);
    copy[length] = 0;

    char *cursor = copy + strlen("a=candidate:");
    char *save = NULL;

    for (char *token = strtok_r(cursor, " ", &save);
         token != NULL && count < 8;
         token = strtok_r(NULL, " ", &save)) {
        fields[count++] = token;
    }

    if (count < 8) {
        return 0;
    }

    const char *foundation = fields[0];
    const char *component = fields[1];
    const char *transport = fields[2];
    const char *priority = fields[3];
    const char *address = fields[4];
    const char *port = fields[5];
    const char *typ = fields[6];
    const char *type = fields[7];

    if (!valid_ice_token(foundation, strlen(foundation), LP_FOUNDATION_MAX) ||
        strcmp(component, "1") != 0 ||
        (strcmp(transport, "udp") != 0 && strcmp(transport, "UDP") != 0) ||
        !all_digits(priority, 10) || strtoull(priority, NULL, 10) > 0xFFFFFFFFull ||
        !all_digits(port, 5) || strtoul(port, NULL, 10) == 0 ||
        strtoul(port, NULL, 10) > 65535 || strcmp(typ, "typ") != 0 ||
        (strcmp(type, "host") != 0 && strcmp(type, "srflx") != 0 &&
         strcmp(type, "relay") != 0)) {
        return 0;
    }

    struct in_addr ipv4;

    if (strlen(address) <= LP_ADDRESS_MAX &&
        inet_pton(AF_INET, address, &ipv4) == 1) {
        *is_mdns = 0;
        return 1;
    }

    if (valid_mdns_name(address)) {
        *is_mdns = 1;
        return 1;
    }

    return 0;       /* IPv6 (libpeer is built without it) or garbage */
}

static int valid_fingerprint(const char *value, size_t length)
{
    if (length != LP_FINGERPRINT_LENGTH) {
        return 0;
    }

    for (size_t i = 0; i < length; i++) {
        int c = (unsigned char) value[i];

        if (i % 3 == 2 ? c != ':' : !isxdigit(c)) {
            return 0;
        }
    }

    return 1;
}

int lp_sdp_sanitize_answer(const char *answer,
                           char *out_buffer,
                           size_t out_size,
                           LpSdpAnswerInfo *info,
                           char *error,
                           size_t error_size)
{
    LpSdpAnswerInfo local_info;

    if (info == NULL) {
        info = &local_info;
    }

    memset(info, 0, sizeof(*info));

    if (answer == NULL || out_buffer == NULL || out_size == 0) {
        return fail(error, error_size, "no answer");
    }

    size_t input_length = strlen(answer);

    if (input_length == 0) {
        return fail(error, error_size, "empty answer");
    }

    if (input_length > LP_SDP_MAX_INPUT) {
        return fail(error, error_size, "answer too large (%zu bytes, limit %d)",
                    input_length, LP_SDP_MAX_INPUT);
    }

    Out out = { out_buffer, out_size, 0, 0 };
    out_buffer[0] = 0;

    int have_version = 0;
    int have_ufrag = 0;
    int have_pwd = 0;
    int have_fingerprint = 0;
    int have_video = 0;
    int setup_active = 0;
    int line_number = 0;

    const char *cursor = answer;

    while (*cursor != 0) {
        const char *end = strchr(cursor, '\n');
        size_t length = end != NULL ? (size_t) (end - cursor) : strlen(cursor);
        const char *next = end != NULL ? end + 1 : cursor + length;

        line_number++;

        if (length > 0 && cursor[length - 1] == '\r') {
            length--;
        }

        if (length == 0) {
            cursor = next;
            continue;
        }

        if (length > LP_SDP_MAX_LINE) {
            return fail(error, error_size, "line %d is %zu bytes long "
                        "(limit %d)", line_number, length, LP_SDP_MAX_LINE);
        }

        for (size_t i = 0; i < length; i++) {
            unsigned char c = (unsigned char) cursor[i];

            if (c < 0x20 && c != '\t') {
                return fail(error, error_size, "line %d contains a control "
                            "character", line_number);
            }
        }

        if (starts_with(cursor, length, "v=0")) {
            have_version = 1;
        } else if (starts_with(cursor, length, "a=ice-ufrag:")) {
            size_t n = length - strlen("a=ice-ufrag:");

            if (!valid_ice_token(cursor + strlen("a=ice-ufrag:"), n,
                                 LP_UFRAG_MAX)) {
                return fail(error, error_size, "invalid a=ice-ufrag");
            }
            have_ufrag = 1;
        } else if (starts_with(cursor, length, "a=ice-pwd:")) {
            size_t n = length - strlen("a=ice-pwd:");

            if (!valid_ice_token(cursor + strlen("a=ice-pwd:"), n, LP_PWD_MAX)) {
                return fail(error, error_size, "invalid a=ice-pwd");
            }
            have_pwd = 1;
        } else if (starts_with(cursor, length, "a=fingerprint:")) {
            size_t prefix = strlen(LP_FINGERPRINT_PREFIX);

            if (!starts_with(cursor, length, LP_FINGERPRINT_PREFIX) ||
                !valid_fingerprint(cursor + prefix, length - prefix)) {
                return fail(error, error_size, "the DTLS fingerprint must be "
                            "sha-256 (\"a=fingerprint:sha-256 AB:CD:...\")");
            }

            /* libpeer compares with its own upper case "%.2X" digest. */
            char line[LP_SDP_MAX_LINE + 1];

            memcpy(line, cursor, length);
            for (size_t i = prefix; i < length; i++) {
                line[i] = (char) toupper((unsigned char) line[i]);
            }

            out_line(&out, line, length);
            have_fingerprint = 1;
            cursor = next;
            continue;
        } else if (starts_with(cursor, length, "a=setup:")) {
            if (!starts_with(cursor, length, "a=setup:active")) {
                return fail(error, error_size, "the answer must take the DTLS "
                            "client role (a=setup:active)");
            }
            setup_active = 1;
        } else if (starts_with(cursor, length, "m=video ")) {
            if (starts_with(cursor, length, "m=video 0 ")) {
                return fail(error, error_size, "the browser rejected the "
                            "video stream (H.264 not supported?)");
            }
            have_video = 1;
        } else if (starts_with(cursor, length, "a=candidate:")) {
            int is_mdns = 0;

            if (info->candidates_kept >= LP_SDP_MAX_CANDIDATES ||
                !usable_candidate(cursor, length, &is_mdns)) {
                info->candidates_dropped++;
                cursor = next;
                continue;
            }

            info->candidates_kept++;
            info->mdns_candidates += is_mdns;
        }

        out_line(&out, cursor, length);
        cursor = next;
    }

    if (!have_version) {
        return fail(error, error_size, "not an SDP document (no v=0)");
    }

    if (!have_video) {
        return fail(error, error_size, "the answer has no video stream");
    }

    if (!have_ufrag || !have_pwd) {
        return fail(error, error_size, "the answer has no ICE credentials");
    }

    if (!have_fingerprint) {
        return fail(error, error_size, "the answer has no DTLS fingerprint");
    }

    if (!setup_active) {
        return fail(error, error_size, "the answer has no a=setup:active");
    }

    if (info->candidates_kept == 0) {
        return fail(error, error_size, "the answer has no usable ICE "
                    "candidate (need UDP over IPv4 or an mDNS .local host; "
                    "%d dropped). Send the answer after ICE gathering "
                    "completes", info->candidates_dropped);
    }

    if (out.overflow) {
        return fail(error, error_size, "answer does not fit the %zu byte "
                    "buffer", out_size);
    }

    return 0;
}
