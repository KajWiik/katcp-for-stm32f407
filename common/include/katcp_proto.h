#ifndef KATCP_PROTO_H
#define KATCP_PROTO_H

/*
 * Minimal, transport-agnostic KATCP line protocol.
 *
 * This module knows nothing about TCP, W5500, or the Pico SDK. It is fed
 * raw bytes as they arrive (katcp_feed), and calls kp->out(...) with
 * complete reply/inform lines whenever a request has been fully parsed
 * and dispatched. The caller (main.c) is responsible for actually
 * writing kp->out()'s bytes to the socket.
 *
 * Wire format reference: NRF-KAT7-6.0-IFCE-002 (KATCP guidelines).
 *   ?name arg1 arg2 ...\n   request
 *   !name status arg...\n  reply      (status: ok | fail | invalid)
 *   #name arg...\n         inform     (sync, inside a reply, or async)
 *
 * Escaping inside arguments (single backslash-prefixed):
 *   \_  space   \n  LF   \r  CR   \t  TAB   \e  ESC(0x1b)
 *   \0  NUL     \@  empty-argument marker (zero-width)   \\  backslash
 */

#include <stddef.h>
#include <stdint.h>

#define KATCP_MAX_LINE      256   /* longest single incoming line we accept   */
#define KATCP_MAX_ARGS      16    /* max tokens in one request                */
#define KATCP_MAX_CMD_NAME  32

typedef struct katcp_ctx katcp_ctx_t;

/* Called by the protocol layer to emit bytes (a reply or inform line,
 * newline included). The implementation just appends to a TX buffer /
 * calls send() -- it must not re-enter katcp_feed(). */
typedef void (*katcp_out_fn)(void *user, const char *data, size_t len);

/* argv[0] is the command name without the leading '?'. Arguments are
 * already unescaped. Handler replies via katcp_reply()/katcp_inform()
 * below, using the ctx pointer passed to it. */
typedef void (*katcp_handler_fn)(katcp_ctx_t *ctx, int argc, char **argv, void *user);

typedef struct {
    const char       *name;    /* e.g. "watchdog", "synth-frequency" */
    const char       *help;    /* one-line description for ?help     */
    katcp_handler_fn  fn;
} katcp_cmd_t;

struct katcp_ctx {
    /* --- fixed at init --- */
    const katcp_cmd_t *cmds;
    int                ncmds;
    katcp_out_fn       out;
    void              *user;         /* opaque, passed to out() and handlers */

    /* --- line accumulation state --- */
    char     linebuf[KATCP_MAX_LINE];
    size_t   linelen;

    /* --- set while a handler is running, so katcp_reply/inform know
     *     which request they're responding to (for message IDs) --- */
    const char *cur_name;
    const char *cur_mid;      /* NULL if request had no [mid] */
};

/* One-time setup. cmds[]/ncmds must outlive ctx. */
void katcp_init(katcp_ctx_t *ctx, const katcp_cmd_t *cmds, int ncmds,
                 katcp_out_fn out, void *user);

/* Feed newly-received bytes. Safe to call with any chunk size (e.g.
 * whatever recv() returned). Processes as many complete lines as are
 * present; partial trailing data is buffered for the next call. */
void katcp_feed(katcp_ctx_t *ctx, const char *data, size_t len);

/* --- helpers a command handler calls from inside its katcp_handler_fn --- */

/* Emit "!<name>[mid] <status> arg1 arg2 ...\n". status is "ok"/"fail"/"invalid". */
void katcp_reply(katcp_ctx_t *ctx, const char *status, int argc, const char **argv);

/* Emit "#<name>[mid] arg1 arg2 ...\n" (a synchronous inform, part of a reply). */
void katcp_inform(katcp_ctx_t *ctx, const char *name, int argc, const char **argv);

/* Emit "#<name> arg1 arg2 ...\n" with no mid -- for unsolicited/async informs
 * (e.g. #synth-locked 0) sent outside of any request/reply cycle. */
void katcp_inform_async(katcp_ctx_t *ctx, const char *name, int argc, const char **argv);

#endif /* KATCP_PROTO_H */
