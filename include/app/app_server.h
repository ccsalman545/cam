/*
 * app_server.h
 *
 * Top level server: wires the capture source, the encoder and the
 * WebRTC transport into one process and runs the network loop.
 *
 * Ownership: app_server_run() allocates all server state, stops the
 * capture and encode threads, closes every session and frees the DTLS
 * context before returning, so a caller only owns the AppConfig it
 * passed in. Everything in the server object, including the sessions
 * and the UDP sockets, belongs to the calling thread: the capture and
 * encode threads receive shared objects (hub, ring, encoder) and the
 * atomics that gate them, never the server itself.
 */
#ifndef APP_APP_SERVER_H
#define APP_APP_SERVER_H

#include <signal.h>

#include "app_config.h"

/*
 * Run until *stop_flag becomes nonzero. The configuration is taken by
 * pointer because /api/config/reload may update the runtime parts of
 * it. Returns the process exit code.
 */
int app_server_run(AppConfig *config, volatile sig_atomic_t *stop_flag);

#endif
