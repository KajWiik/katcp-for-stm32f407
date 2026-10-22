#include "web_ui.h"
#include "sensor.h"
#include "synth.h"
#include "pac1954.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Last frequency successfully programmed from this page (0 = none
 * yet). Requests made over KATCP (?synth-frequency) don't update it --
 * synth.h has no getter, and this deliberately doesn't add one just
 * for display. */
static uint64_t last_set_hz;

/* ---- the page -------------------------------------------------------------
 * Self-contained (no external scripts/fonts -- the board may sit on a
 * network with no internet access). Polls /api/status once a second. */
static const char page_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Synth Control</title><style>"
"body{font-family:system-ui,sans-serif;margin:16px;max-width:640px;color:#222;background:#fafafa}"
"h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.5em}"
"input{font-size:1em;padding:4px;width:12em}button{font-size:1em;padding:4px 12px}"
"table{border-collapse:collapse;width:100%}th,td{padding:4px 8px;border-bottom:1px solid #ddd;text-align:right}"
"th:first-child,td:first-child{text-align:left}.err{color:#b00}.ok{color:#070}"
"#msg{min-height:1.2em}"
"</style></head><body>"
"<h1>Synth Control</h1>"
"<h2>Synthesizer</h2>"
"<form id=\"f\"><label>Frequency (MHz) <input id=\"mhz\" type=\"number\" step=\"any\" required></label> "
"<button>Set</button></form>"
"<p id=\"msg\"></p>"
"<p>PLL: <b id=\"lock\">?</b> &middot; last set here: <b id=\"last\">-</b></p>"
"<h2>PAC1954</h2>"
"<table><thead><tr><th>Channel</th><th>Voltage (V)</th><th>Current (A)</th><th>Power (W)</th></tr></thead>"
"<tbody id=\"pac\"></tbody></table>"
"<p id=\"upd\" style=\"color:#888;font-size:.85em\"></p>"
"<script>"
"const $=id=>document.getElementById(id);"
"const fmt=(v,d)=>v===null?'&ndash;':v.toFixed(d);"
"async function poll(){try{"
"const s=await (await fetch('/api/status',{cache:'no-store'})).json();"
"$('lock').textContent=s.synth.locked?'locked':'unlocked';"
"$('lock').className=s.synth.locked?'ok':'err';"
"$('last').textContent=s.synth.last_set_hz?(s.synth.last_set_hz/1e6)+' MHz':'-';"
"$('pac').innerHTML=s.pac1954.map(c=>c.status==='nominal'"
"?'<tr><td>'+c.channel+'</td><td>'+fmt(c.voltage,3)+'</td><td>'+fmt(c.current,4)+'</td><td>'+fmt(c.power,3)+'</td></tr>'"
":'<tr><td>'+c.channel+'</td><td colspan=3 class=err>'+c.status+'</td></tr>').join('');"
"$('upd').textContent='updated '+new Date().toLocaleTimeString();"
"}catch(e){$('upd').textContent='board not responding';}}"
"$('f').onsubmit=async e=>{e.preventDefault();"
"const hz=Math.round(parseFloat($('mhz').value)*1e6);"
"$('msg').className='';$('msg').textContent='setting...';"
"try{const r=await (await fetch('/api/frequency?hz='+hz,{cache:'no-store'})).json();"
"$('msg').className=r.ok?'ok':'err';"
"$('msg').textContent=r.ok?'set to '+(r.hz/1e6)+' MHz':'failed: '+r.error;"
"}catch(e){$('msg').className='err';$('msg').textContent='request failed';}poll();};"
"poll();setInterval(poll,1000);"
"</script></body></html>";

/* ---- response helpers ----------------------------------------------------- */

static void out_str(web_ui_out_fn out, void *user, const char *s)
{
    out(user, s, strlen(s));
}

static void send_head(web_ui_out_fn out, void *user, const char *status,
                      const char *content_type)
{
    char head[192];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n", status, content_type);
    out(user, head, (size_t)n);
}

static void send_json(web_ui_out_fn out, void *user, const char *status,
                      const char *body)
{
    send_head(out, user, status, "application/json");
    out_str(out, user, body);
}

/* ---- sensor lookup -------------------------------------------------------- */

/* Reads a sensor by name through sensor_table, so the web page reports
 * exactly what ?sensor-value would (same conversions, same shunt
 * config). Only synth.locked and pac1954.* are read from here:
 * mcu.temperature drives ADC1 directly and isn't safe to call from a
 * second thread concurrently with the KATCP one. */
static sensor_status_t read_by_name(const char *name, sensor_value_t *v)
{
    for (int i = 0; i < sensor_table_len; i++) {
        if (strcmp(sensor_table[i].name, name) == 0)
            return sensor_table[i].read(v);
    }
    return SENSOR_STATUS_UNKNOWN;
}

/* Appends printf-style text at buf+*len, tracking overflow in *ok. */
static void appendf(char *buf, size_t cap, size_t *len, bool *ok, const char *fmt, ...)
{
    if (!*ok)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) {
        *ok = false;
        return;
    }
    *len += (size_t)n;
}

/* ---- routes --------------------------------------------------------------- */

static void route_status(web_ui_out_fn out, void *user)
{
    static const char *const quantity[] = { "voltage", "current", "power" };
    char body[768];
    size_t len = 0;
    bool ok = true;

    sensor_value_t v = {0};
    read_by_name("synth.locked", &v);
    appendf(body, sizeof(body), &len, &ok,
            "{\"synth\":{\"locked\":%s,\"last_set_hz\":%llu},\"pac1954\":[",
            v.i ? "true" : "false", (unsigned long long)last_set_hz);

    for (int ch = 1; ch <= PAC1954_NUM_CHANNELS; ch++) {
        appendf(body, sizeof(body), &len, &ok, "%s{\"channel\":%d",
                ch > 1 ? "," : "", ch);
        sensor_status_t worst = SENSOR_STATUS_NOMINAL;
        for (int q = 0; q < 3; q++) {
            char name[32];
            snprintf(name, sizeof(name), "pac1954.ch%d.%s", ch, quantity[q]);
            sensor_value_t qv = {0};
            sensor_status_t st = read_by_name(name, &qv);
            if (st != SENSOR_STATUS_NOMINAL) {
                worst = st;
                appendf(body, sizeof(body), &len, &ok, ",\"%s\":null", quantity[q]);
            } else {
                appendf(body, sizeof(body), &len, &ok, ",\"%s\":%.6f", quantity[q], qv.f);
            }
        }
        appendf(body, sizeof(body), &len, &ok, ",\"status\":\"%s\"}",
                worst == SENSOR_STATUS_NOMINAL ? "nominal" :
                worst == SENSOR_STATUS_ERROR   ? "error"   : "unknown");
    }
    appendf(body, sizeof(body), &len, &ok, "]}");

    if (!ok) {
        send_json(out, user, "500 Internal Server Error",
                  "{\"error\":\"status too large\"}");
        return;
    }
    send_json(out, user, "200 OK", body);
}

/* Strictly parses the hz=<digits> query parameter. Rejects anything
 * that isn't 1-15 plain decimal digits (15 GHz is 11), so there's no
 * overflow or sign/whitespace leniency from strtoull to worry about. */
static bool parse_hz(const char *query, uint64_t *hz)
{
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, "hz=", 3) == 0) {
            p += 3;
            uint64_t v = 0;
            int digits = 0;
            while (*p >= '0' && *p <= '9') {
                if (++digits > 15)
                    return false;
                v = v * 10 + (uint64_t)(*p++ - '0');
            }
            if (digits == 0 || (*p != '\0' && *p != '&'))
                return false;
            *hz = v;
            return true;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return false;
}

static void route_frequency(const char *query, web_ui_out_fn out, void *user)
{
    uint64_t hz;
    if (!query || !parse_hz(query, &hz)) {
        send_json(out, user, "400 Bad Request",
                  "{\"ok\":false,\"error\":\"expected hz=<integer>\"}");
        return;
    }
    if (!synth_set_frequency(hz)) {
        send_json(out, user, "200 OK",
                  "{\"ok\":false,\"error\":\"synth programming failed or out of range\"}");
        return;
    }
    last_set_hz = hz;

    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"hz\":%llu}", (unsigned long long)hz);
    send_json(out, user, "200 OK", body);
}

/* ---- entry point ---------------------------------------------------------- */

void web_ui_handle_request(const char *request, web_ui_out_fn out, void *user)
{
    /* Request line: METHOD SP TARGET SP VERSION. Copy the target out
     * so the query can be split off without touching the caller's
     * buffer. */
    char target[128];
    const char *sp1 = strchr(request, ' ');
    const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || (size_t)(sp2 - sp1 - 1) >= sizeof(target)) {
        send_head(out, user, "400 Bad Request", "text/plain");
        out_str(out, user, "bad request\n");
        return;
    }
    size_t tlen = (size_t)(sp2 - sp1 - 1);
    memcpy(target, sp1 + 1, tlen);
    target[tlen] = '\0';

    if ((size_t)(sp1 - request) != 3 || strncmp(request, "GET", 3) != 0) {
        send_head(out, user, "405 Method Not Allowed", "text/plain");
        out_str(out, user, "only GET is supported\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query)
        *query++ = '\0';

    if (strcmp(target, "/") == 0) {
        send_head(out, user, "200 OK", "text/html; charset=utf-8");
        out(user, page_html, sizeof(page_html) - 1);
    } else if (strcmp(target, "/api/status") == 0) {
        route_status(out, user);
    } else if (strcmp(target, "/api/frequency") == 0) {
        route_frequency(query, out, user);
    } else {
        send_head(out, user, "404 Not Found", "text/plain");
        out_str(out, user, "not found\n");
    }
}
