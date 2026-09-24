/* server.c - minimal portable HTTP static file server for embedded Linux
 * One file, one dependency (mongoose.c/h), no runtime, no container.
 * Listens 0.0.0.0:8000 and serves ./web_root
 * Build: gcc -O2 -Wall server.c mongoose.c -o web_server
 */
#include "mongoose.h"

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_serve_opts opts = {.root_dir = "./web_root"};
    mg_http_serve_dir(c, (struct mg_http_message *) ev_data, &opts);
  }
}

int main(void) {
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  if (mg_http_listen(&mgr, "http://0.0.0.0:8000", ev_handler, NULL) == NULL) {
    return 1;
  }
  for (;;) mg_mgr_poll(&mgr, 1000);
  mg_mgr_free(&mgr);
  return 0;
}
