/*
 * lp_server.h
 *
 * camstream-libpeer: the camstream capture and encode pipeline with
 * libpeer (github.com/sepfy/libpeer) as the WebRTC stack and mongoose as
 * the HTTP signaling server.
 *
 *   camera -> source thread -> frame hub -> encoder thread -> AU ring
 *          -> media thread -> one libpeer session thread per viewer
 *
 * HTTP API (JSON):
 *   GET  /                    viewer page
 *   POST /api/session         -> {"id":N,"sdp":"<offer>"}
 *   POST /api/session/answer  {"id":N,"sdp":"<answer>"} (candidates inside)
 *   POST /api/session/close   {"id":N}
 *   GET  /api/status          pipeline and per-viewer state
 */
#ifndef LPSTREAM_LP_SERVER_H
#define LPSTREAM_LP_SERVER_H

#include <signal.h>

#include "app_config.h"

/* Viewers served at once; each costs one thread and one encrypted copy. */
#define LP_MAX_SESSIONS 4

/* Runs until *stop_flag is set. Returns the process exit status. */
int lp_server_run(const AppConfig *config, volatile sig_atomic_t *stop_flag);

#endif
