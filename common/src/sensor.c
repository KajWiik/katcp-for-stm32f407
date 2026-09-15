#include "sensor.h"
#include "synth.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- individual sensor read functions ----------------------------------- */

static sensor_status_t read_synth_locked(int64_t *out)
{
    *out = synth_is_locked() ? 1 : 0;
    /* The read itself always succeeds (it's just a GPIO/status query);
     * "unlocked" is a normal, nominal state to report while the synth
     * is unprogrammed or settling, not a sensor failure. Whether an
     * unlocked synth should ever be reported as SENSOR_STATUS_WARN
     * during normal operation is a policy call left open here. */
    return SENSOR_STATUS_NOMINAL;
}

static sensor_status_t read_uptime_s(int64_t *out)
{
    *out = (int64_t)(platform_now_ms() / 1000);
    return SENSOR_STATUS_NOMINAL;
}

static sensor_status_t read_mcu_temperature(int64_t *out)
{
    *out = platform_mcu_millidegrees_c();
    /* Uncalibrated die-temperature estimate (see platform.h) -- the
     * read itself doesn't fail, it's just rough. Nothing here treats
     * "unusually high" as SENSOR_STATUS_WARN; that would need a
     * threshold policy decision this project hasn't made. */
    return SENSOR_STATUS_NOMINAL;
}

/* ---- the table ------------------------------------------------------------ */

const sensor_def_t sensor_table[] = {
    { "synth.locked", "Synthesizer PLL lock status", "",
      SENSOR_BOOLEAN, 0, 1, 0, read_synth_locked },
    { "board.uptime", "Seconds since boot", "seconds",
      SENSOR_INTEGER, 0, INT64_MAX, 0, read_uptime_s },
    { "mcu.temperature", "STM32F407 die temperature (uncalibrated ADC estimate)", "degrees_C",
      SENSOR_FLOAT, -40000, 125000, 1000, read_mcu_temperature },
};
const int sensor_table_len = sizeof(sensor_table) / sizeof(sensor_table[0]);

/* ---- formatting helpers --------------------------------------------------- */

static const char *type_name(sensor_type_t t)
{
    switch (t) {
        case SENSOR_BOOLEAN: return "boolean";
        case SENSOR_INTEGER: return "integer";
        case SENSOR_FLOAT:   return "float";
        default:             return "unknown";
    }
}

static const char *status_name(sensor_status_t s)
{
    switch (s) {
        case SENSOR_STATUS_NOMINAL: return "nominal";
        case SENSOR_STATUS_WARN:    return "warn";
        case SENSOR_STATUS_ERROR:   return "error";
        default:                    return "unknown";
    }
}

/* Formats raw (already scaled by `scale`, a positive power of 10) as
 * a fixed-point decimal string -- e.g. raw=23456, scale=1000 ->
 * "23.456". Never uses %f/float/double anywhere (see sensor.h).
 * Handles negative values via the "negate then add 1" trick so it's
 * also correct (no UB, no wraparound) for raw == INT64_MIN, though
 * nothing in this codebase's actual sensor ranges gets near there.
 * Verified against 10 hand-picked cases (zero, exact boundaries,
 * smallest-magnitude positive/negative, INT64_MIN) before being used
 * here, including the negative-value formatting pitfall this project
 * flagged as a known risk when scaled-integer floats were first
 * discussed ("-5.234" printing as "-5.-234" from a naive llabs()). */
static void format_fixed(int64_t raw, int64_t scale, char *buf, size_t buflen)
{
    int digits = 0;
    for (int64_t s = scale; s > 1; s /= 10) digits++;

    bool neg = raw < 0;
    uint64_t mag = neg ? (uint64_t)(-(raw + 1)) + 1 : (uint64_t)raw;
    uint64_t whole = mag / (uint64_t)scale;
    uint64_t frac  = mag % (uint64_t)scale;

    snprintf(buf, buflen, "%s%llu.%0*llu", neg ? "-" : "",
             (unsigned long long)whole, digits, (unsigned long long)frac);
}

static void find_sensor(const char *name, const sensor_def_t **out)
{
    for (int i = 0; i < sensor_table_len; i++) {
        if (strcmp(sensor_table[i].name, name) == 0) {
            *out = &sensor_table[i];
            return;
        }
    }
    *out = NULL;
}

/* ---- ?sensor-list --------------------------------------------------------- */

static void emit_sensor_list_entry(katcp_ctx_t *ctx, const sensor_def_t *s)
{
    if (s->type == SENSOR_INTEGER) {
        char minbuf[24], maxbuf[24];
        snprintf(minbuf, sizeof(minbuf), "%lld", (long long)s->min);
        snprintf(maxbuf, sizeof(maxbuf), "%lld", (long long)s->max);
        const char *info[] = { s->name, s->description, s->units,
                                type_name(s->type), minbuf, maxbuf };
        katcp_inform(ctx, "sensor-list", 6, info);
    } else if (s->type == SENSOR_FLOAT) {
        char minbuf[32], maxbuf[32];
        format_fixed(s->min, s->scale, minbuf, sizeof(minbuf));
        format_fixed(s->max, s->scale, maxbuf, sizeof(maxbuf));
        const char *info[] = { s->name, s->description, s->units,
                                type_name(s->type), minbuf, maxbuf };
        katcp_inform(ctx, "sensor-list", 6, info);
    } else {
        const char *info[] = { s->name, s->description, s->units, type_name(s->type) };
        katcp_inform(ctx, "sensor-list", 4, info);
    }
}

void cmd_sensor_list(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)user;

    if (argc == 1) {
        for (int i = 0; i < sensor_table_len; i++)
            emit_sensor_list_entry(ctx, &sensor_table[i]);
        char count[8];
        snprintf(count, sizeof(count), "%d", sensor_table_len);
        const char *n[] = { count };
        katcp_reply(ctx, "ok", 1, n);
        return;
    }

    const sensor_def_t *s;
    find_sensor(argv[1], &s);
    if (!s) {
        const char *msg[] = { "unknown sensor" };
        katcp_reply(ctx, "fail", 1, msg);
        return;
    }
    emit_sensor_list_entry(ctx, s);
    const char *n[] = { "1" };
    katcp_reply(ctx, "ok", 1, n);
}

/* ---- ?sensor-value --------------------------------------------------------- */

static void emit_sensor_value_entry(katcp_ctx_t *ctx, const sensor_def_t *s)
{
    int64_t v = 0;
    sensor_status_t st = s->read(&v);

    /* KATCP v5 specifies sensor timestamps as UTC seconds, not device
     * uptime (see IMPLEMENTATION.md §7 for the full discussion). On a
     * target with no synced wall clock (or before the first sync
     * completes), platform_wallclock_unix_s() returns 0 -- an honest
     * "unsynced" signal (1970-01-01) rather than a plausible-looking
     * but meaningless boot-relative number. */
    char tsbuf[24], valbuf[32];
    snprintf(tsbuf, sizeof(tsbuf), "%lu", (unsigned long)platform_wallclock_unix_s());
    if (s->type == SENSOR_BOOLEAN)
        snprintf(valbuf, sizeof(valbuf), "%d", v ? 1 : 0);
    else if (s->type == SENSOR_FLOAT)
        format_fixed(v, s->scale, valbuf, sizeof(valbuf));
    else
        snprintf(valbuf, sizeof(valbuf), "%lld", (long long)v);

    const char *info[] = { tsbuf, "1", s->name, status_name(st), valbuf };
    katcp_inform(ctx, "sensor-value", 5, info);
}

void cmd_sensor_value(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)user;

    if (argc == 1) {
        for (int i = 0; i < sensor_table_len; i++)
            emit_sensor_value_entry(ctx, &sensor_table[i]);
        char count[8];
        snprintf(count, sizeof(count), "%d", sensor_table_len);
        const char *n[] = { count };
        katcp_reply(ctx, "ok", 1, n);
        return;
    }

    const sensor_def_t *s;
    find_sensor(argv[1], &s);
    if (!s) {
        const char *msg[] = { "unknown sensor" };
        katcp_reply(ctx, "fail", 1, msg);
        return;
    }
    emit_sensor_value_entry(ctx, s);
    const char *n[] = { "1" };
    katcp_reply(ctx, "ok", 1, n);
}
