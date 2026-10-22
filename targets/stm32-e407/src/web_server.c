/*
 * web_server.c -- HTTP transport for common/src/web_ui.c on the
 * STM32-E407 (IMPLEMENTATION.md §11). Same shape as the KATCP thread in
 * main.c: lwIP blocking sockets, its own ChibiOS thread, one
 * connection at a time. This file only accepts, reads the request head
 * and closes; everything HTTP-specific beyond that lives in web_ui.c.
 */

#include <string.h>

#include "ch.h"
#include "hal.h"
#include "lwip/sockets.h"

#include "web_ui.h"
#include "web_server.h"

#define WEB_PORT 80

/* Browsers open speculative connections they may never send a request
 * on; with a single-connection server, one of those would block every
 * other client indefinitely. SO_RCVTIMEO (LWIP_SO_RCVTIMEO=1 in
 * cfg/lwipopts.h) bounds that. */
#define WEB_RECV_TIMEOUT_MS 2000

static void web_out_to_socket(void *user, const char *data, size_t len)
{
    int sock = *(int *)user;
    while (len > 0) {
        int n = send(sock, data, len, 0);
        if (n <= 0) {
            return; /* client went away -- nothing useful to do */
        }
        data += n;
        len  -= (size_t)n;
    }
}

/* Reads until the end of the request head ("\r\n\r\n"), the buffer
 * fills, or the peer stops sending. Returns bytes read (NUL-terminated),
 * or 0 if nothing usable arrived. Request bodies are never needed --
 * every route is a GET. */
static size_t read_request_head(int sock, char *buf, size_t cap)
{
    size_t len = 0;
    while (len < cap - 1) {
        int got = recv(sock, buf + len, cap - 1 - len, 0);
        if (got <= 0) {
            break;
        }
        len += (size_t)got;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) {
            return len;
        }
    }
    buf[len] = '\0';
    /* A head that didn't fit is still worth answering if the request
     * line itself arrived -- that's all web_ui.c looks at. */
    return strstr(buf, "\r\n") != NULL ? len : 0;
}

static THD_WORKING_AREA(wa_web_thread, 3072);
static THD_FUNCTION(web_thread, arg)
{
    (void)arg;
    chRegSetThreadName("http");

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(WEB_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_sock, 2);

    static char req[1024];
    for (;;) {
        int client = accept(listen_sock, NULL, NULL);
        if (client < 0) {
            continue;
        }

        struct timeval tv = {
            .tv_sec  = WEB_RECV_TIMEOUT_MS / 1000,
            .tv_usec = (WEB_RECV_TIMEOUT_MS % 1000) * 1000,
        };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (read_request_head(client, req, sizeof(req)) > 0) {
            web_ui_handle_request(req, web_out_to_socket, &client);
        }
        close(client);
    }
}

void web_server_start(void)
{
    chThdCreateStatic(wa_web_thread, sizeof(wa_web_thread),
                      NORMALPRIO - 1, web_thread, NULL);
}
