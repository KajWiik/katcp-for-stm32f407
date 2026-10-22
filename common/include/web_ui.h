#ifndef WEB_UI_H
#define WEB_UI_H

#include <stddef.h>

/* Minimal browser front-end (IMPLEMENTATION.md §11): one page with a
 * frequency input per synth and a live PAC1954 readout, plus two small
 * JSON endpoints it calls. Transport-agnostic in the same way as
 * katcp_proto.h -- this only turns one request's text into response
 * bytes via `out`; the target's socket glue (targets/stm32-e407/src/
 * web_server.c) owns accept/recv/close, and the desktop build can drive it
 * directly from a string.
 *
 * Routes (GET only, everything else gets 404/405):
 *   /                      the HTML page
 *   /api/status            JSON: per-synth lock + last frequency set here,
 *                          and voltage/current/power for each PAC1954 channel
 *   /api/frequency?synth=<1..SYNTH_COUNT>&hz=<n>
 *                          calls synth_set_frequency(synth-1, n), JSON result
 */

typedef void (*web_ui_out_fn)(void *user, const char *data, size_t len);

/* `request` is the request head (request line + headers), NUL-
 * terminated; any body is ignored. Writes one complete HTTP/1.0-style
 * response (Connection: close, no Content-Length -- the caller closes
 * the socket to end it). */
void web_ui_handle_request(const char *request, web_ui_out_fn out, void *user);

#endif /* WEB_UI_H */
