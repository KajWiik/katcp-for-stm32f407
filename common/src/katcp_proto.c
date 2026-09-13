#include "katcp_proto.h"

#include <string.h>
#include <stdio.h>

/* ---- escaping ------------------------------------------------------- */

/* Unescape one argument in place. Returns new length. */
static size_t unescape_arg(char *s, size_t len)
{
    size_t r = 0, w = 0;
    while (r < len) {
        if (s[r] == '\\' && r + 1 < len) {
            r++;
            switch (s[r]) {
                case 'e': s[w++] = 0x1b; break;
                case 'n': s[w++] = '\n'; break;
                case 'r': s[w++] = '\r'; break;
                case 't': s[w++] = '\t'; break;
                case '0': s[w++] = '\0'; break;
                case '_': s[w++] = ' ';  break;
                case '@': /* empty-argument marker: contributes nothing */ break;
                case '\\': s[w++] = '\\'; break;
                default:  s[w++] = s[r]; break; /* unknown escape: literal char */
            }
            r++;
        } else {
            s[w++] = s[r++];
        }
    }
    return w;
}

static void emit_escaped_arg(katcp_ctx_t *ctx, const char *s, size_t len)
{
    char buf[KATCP_MAX_LINE];
    size_t w = 0;

    if (len == 0) {
        buf[w++] = '\\'; buf[w++] = '@';
    } else {
        for (size_t i = 0; i < len && w + 2 < sizeof(buf); i++) {
            unsigned char c = (unsigned char)s[i];
            switch (c) {
                case ' ':  buf[w++] = '\\'; buf[w++] = '_'; break;
                case '\n': buf[w++] = '\\'; buf[w++] = 'n'; break;
                case '\r': buf[w++] = '\\'; buf[w++] = 'r'; break;
                case '\t': buf[w++] = '\\'; buf[w++] = 't'; break;
                case '\\': buf[w++] = '\\'; buf[w++] = '\\'; break;
                case 0x1b: buf[w++] = '\\'; buf[w++] = 'e'; break;
                case '\0': buf[w++] = '\\'; buf[w++] = '0'; break;
                default:   buf[w++] = (char)c; break;
            }
        }
    }
    ctx->out(ctx->user, buf, w);
}

/* ---- tokenizing a complete line -------------------------------------- */

/* Splits ctx->linebuf[0..linelen) into argv, unescaping in place.
 * argv[0] is the bare command name (no '?'), with any [mid] suffix
 * stripped out and returned separately via *mid_out (points into
 * linebuf, or NULL). Returns argc, or -1 on malformed input. */
static int tokenize(katcp_ctx_t *ctx, char **argv, int max_argv, const char **mid_out)
{
    char *p = ctx->linebuf;
    size_t remaining = ctx->linelen;
    int argc = 0;
    *mid_out = NULL;

    if (remaining == 0 || (*p != '?' && *p != '!' && *p != '#'))
        return -1; /* we only ever expect to *receive* requests ('?') */

    p++; remaining--; /* skip the leading '?' */

    while (remaining > 0 && argc < max_argv) {
        /* skip separating whitespace */
        while (remaining > 0 && *p == ' ') { p++; remaining--; }
        if (remaining == 0) break;

        char *start = p;
        while (remaining > 0 && *p != ' ') {
            if (*p == '\\' && remaining > 1) { p++; remaining--; } /* skip escaped char too */
            p++; remaining--;
        }
        size_t toklen = (size_t)(p - start);

        /* Consume the separator (if any) *before* writing the '\0' below --
         * writing into start[toklen] overwrites the byte p currently
         * points at (when no escaping shrank the token), so p must have
         * already moved past it or the next iteration's whitespace-skip
         * sees a stale '\0' instead of ' ' and misparses. */
        if (remaining > 0) { p++; remaining--; }

        toklen = unescape_arg(start, toklen);
        start[toklen] = '\0';
        argv[argc++] = start;
    }

    if (argc == 0) return -1;

    /* pull [mid] off the end of argv[0], e.g. "halt[3]" -> "halt", mid="3" */
    char *name = argv[0];
    char *bracket = strchr(name, '[');
    if (bracket) {
        char *close = strchr(bracket, ']');
        if (close && close > bracket) {
            *bracket = '\0';
            *close = '\0';
            *mid_out = bracket + 1;
        }
    }
    return argc;
}

/* ---- dispatch --------------------------------------------------------- */

static void dispatch(katcp_ctx_t *ctx)
{
    char *argv[KATCP_MAX_ARGS];
    const char *mid;
    int argc = tokenize(ctx, argv, KATCP_MAX_ARGS, &mid);

    if (argc < 0) {
        /* Malformed line -- nothing sane to reply to, drop it. Real
         * KATCP servers log this; we skip logging to keep things small. */
        return;
    }

    ctx->cur_name = argv[0];
    ctx->cur_mid  = mid;

    for (int i = 0; i < ctx->ncmds; i++) {
        if (strcmp(ctx->cmds[i].name, argv[0]) == 0) {
            ctx->cmds[i].fn(ctx, argc, argv, ctx->user);
            return;
        }
    }

    /* Unknown request: KATCP requires an "invalid" reply, not silence. */
    const char *args[] = { "unknown request" };
    katcp_reply(ctx, "invalid", 1, args);
}

/* ---- public API --------------------------------------------------------- */

void katcp_init(katcp_ctx_t *ctx, const katcp_cmd_t *cmds, int ncmds,
                 katcp_out_fn out, void *user)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cmds  = cmds;
    ctx->ncmds = ncmds;
    ctx->out   = out;
    ctx->user  = user;
}

void katcp_feed(katcp_ctx_t *ctx, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n' || c == '\r') {
            if (ctx->linelen > 0) {
                dispatch(ctx);
                ctx->linelen = 0;
            }
            continue;
        }

        if (ctx->linelen < KATCP_MAX_LINE - 1) {
            ctx->linebuf[ctx->linelen++] = c;
        } else {
            /* Line too long: drop it and resync on the next newline,
             * rather than silently truncating a request. */
            ctx->linelen = 0;
        }
    }
}

static void emit_line(katcp_ctx_t *ctx, char prefix, const char *name, const char *mid,
                       int argc, const char **argv)
{
    char head[KATCP_MAX_CMD_NAME + 8];
    int n;
    if (mid)
        n = snprintf(head, sizeof(head), "%c%s[%s]", prefix, name, mid);
    else
        n = snprintf(head, sizeof(head), "%c%s", prefix, name);
    ctx->out(ctx->user, head, (size_t)n);

    for (int i = 0; i < argc; i++) {
        ctx->out(ctx->user, " ", 1);
        emit_escaped_arg(ctx, argv[i], strlen(argv[i]));
    }
    ctx->out(ctx->user, "\n", 1);
}

void katcp_reply(katcp_ctx_t *ctx, const char *status, int argc, const char **argv)
{
    char full_argv[KATCP_MAX_ARGS + 1][KATCP_MAX_LINE];
    const char *full_ptrs[KATCP_MAX_ARGS + 1];
    full_ptrs[0] = status;
    for (int i = 0; i < argc && i < KATCP_MAX_ARGS; i++) {
        full_ptrs[i + 1] = argv[i];
    }
    (void)full_argv; /* silence unused warning if argc == 0 path taken */
    emit_line(ctx, '!', ctx->cur_name, ctx->cur_mid, argc + 1, full_ptrs);
}

void katcp_inform(katcp_ctx_t *ctx, const char *name, int argc, const char **argv)
{
    emit_line(ctx, '#', name, ctx->cur_mid, argc, argv);
}

void katcp_inform_async(katcp_ctx_t *ctx, const char *name, int argc, const char **argv)
{
    emit_line(ctx, '#', name, NULL, argc, argv);
}
