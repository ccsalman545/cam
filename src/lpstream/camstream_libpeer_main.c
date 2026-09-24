#define _POSIX_C_SOURCE 200809L

/*
 * camstream_libpeer_main.c
 *
 * Entry point of camstream-libpeer: the camstream camera pipeline
 * streamed through libpeer. Takes the same options and config file as
 * camstream (the UDP port range is unused: libpeer picks its own port).
 */
#include <signal.h>
#include <string.h>

#include "app_config.h"
#include "log.h"
#include "lp_server.h"

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int signal_number)
{
    (void) signal_number;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    AppConfig config;

    app_config_defaults(&config);

    int parsed = app_config_parse(&config, argc, argv);

    if (parsed == 1) {
        app_config_print_usage(argv[0]);
        return 0;
    }

    if (parsed != 0) {
        return 2;
    }

    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    /* A viewer that disappears mid-response must not kill the process. */
    signal(SIGPIPE, SIG_IGN);

    /*
     * libpeer binds an ephemeral UDP port per viewer, so the configured
     * range is not used; 0 makes the summary say so (camstream's
     * validation never accepts 0, so the value cannot mean anything else).
     */
    config.udp_base_port = 0;

    log_init(config.verbose ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
    log_info("app", "camstream-libpeer %s starting (WebRTC: libpeer)",
             APP_VERSION);
    app_config_print_summary(&config);

    return lp_server_run(&config, &g_stop);
}
