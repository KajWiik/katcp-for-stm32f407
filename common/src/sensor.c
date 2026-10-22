#include "sensor.h"
#include "synth.h"
#include "platform.h"
#include "pac1954.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- individual sensor read functions ----------------------------------- */

static sensor_status_t read_synth_locked(sensor_value_t *out)
{
    out->i = synth_is_locked() ? 1 : 0;
    /* The read itself always succeeds (it's just a GPIO/status query);
     * "unlocked" is a normal, nominal state to report while the synth
     * is unprogrammed or settling, not a sensor failure. Whether an
     * unlocked synth should ever be reported as SENSOR_STATUS_WARN
     * during normal operation is a policy call left open here. */
    return SENSOR_STATUS_NOMINAL;
}

static sensor_status_t read_uptime_s(sensor_value_t *out)
{
    out->i = (int64_t)(platform_now_ms() / 1000);
    return SENSOR_STATUS_NOMINAL;
}

static sensor_status_t read_mcu_temperature(sensor_value_t *out)
{
    out->f = platform_mcu_temperature_c();
    /* Uncalibrated die-temperature estimate (see platform.h) -- the
     * read itself doesn't fail, it's just rough. Nothing here treats
     * "unusually high" as SENSOR_STATUS_WARN; that would need a
     * threshold policy decision this project hasn't made. */
    return SENSOR_STATUS_NOMINAL;
}

/* PAC1954 power monitor (IMPLEMENTATION.md §6.4). Unlike the sensors
 * above, these reads genuinely can fail (chip absent, I2C error, stale
 * snapshot) -- reported as SENSOR_STATUS_ERROR with a 0 value rather
 * than a plausible-looking stale number. Voltage, current and power
 * for one channel all come from the same refresh, so power is
 * consistent with the other two rather than computed across reads. */
static const double pac1954_rsense_ohms[PAC1954_NUM_CHANNELS] = {
    PAC1954_RSENSE_CH1_OHMS, PAC1954_RSENSE_CH2_OHMS,
    PAC1954_RSENSE_CH3_OHMS, PAC1954_RSENSE_CH4_OHMS,
};

typedef enum { PAC_VOLTAGE, PAC_CURRENT, PAC_POWER } pac_quantity_t;

static sensor_status_t read_pac1954(int ch, pac_quantity_t q, sensor_value_t *out)
{
    uint16_t vbus_code, vsense_code;
    if (!pac1954_read_raw(ch, &vbus_code, &vsense_code)) {
        out->f = 0.0;
        return SENSOR_STATUS_ERROR;
    }
    double v = pac1954_vbus_volts(vbus_code);
    double i = pac1954_vsense_volts(vsense_code) / pac1954_rsense_ohms[ch];
    switch (q) {
        case PAC_VOLTAGE: out->f = v;     break;
        case PAC_CURRENT: out->f = i;     break;
        case PAC_POWER:   out->f = v * i; break;
    }
    return SENSOR_STATUS_NOMINAL;
}

/* sensor_read_fn takes no user argument, so each (channel, quantity)
 * pair needs its own thin wrapper. */
#define PAC1954_READERS(n)                                                     \
    static sensor_status_t read_pac1954_ch##n##_voltage(sensor_value_t *out)   \
    { return read_pac1954((n) - 1, PAC_VOLTAGE, out); }                        \
    static sensor_status_t read_pac1954_ch##n##_current(sensor_value_t *out)   \
    { return read_pac1954((n) - 1, PAC_CURRENT, out); }                        \
    static sensor_status_t read_pac1954_ch##n##_power(sensor_value_t *out)     \
    { return read_pac1954((n) - 1, PAC_POWER, out); }

PAC1954_READERS(1)
PAC1954_READERS(2)
PAC1954_READERS(3)
PAC1954_READERS(4)

/* ---- the table ------------------------------------------------------------ */

/* Ranges are the chip's full-scale limits for the configured shunt
 * (unipolar defaults, see pac1954.h), not expected operating ranges. */
#define PAC1954_IMAX_A(n) (PAC1954_VSENSE_FSR_V / PAC1954_RSENSE_CH##n##_OHMS)
#define PAC1954_SENSORS(n)                                                     \
    { "pac1954.ch" #n ".voltage", "PAC1954 channel " #n " bus voltage", "volts", \
      SENSOR_FLOAT, {.f = 0.0}, {.f = PAC1954_VBUS_FSR_V}, 4,                  \
      read_pac1954_ch##n##_voltage },                                          \
    { "pac1954.ch" #n ".current", "PAC1954 channel " #n " current", "amperes", \
      SENSOR_FLOAT, {.f = 0.0}, {.f = PAC1954_IMAX_A(n)}, 4,                   \
      read_pac1954_ch##n##_current },                                          \
    { "pac1954.ch" #n ".power", "PAC1954 channel " #n " power", "watts",       \
      SENSOR_FLOAT, {.f = 0.0}, {.f = PAC1954_VBUS_FSR_V * PAC1954_IMAX_A(n)}, \
      4, read_pac1954_ch##n##_power }

const sensor_def_t sensor_table[] = {
    { "synth.locked", "Synthesizer PLL lock status", "",
      SENSOR_BOOLEAN, {0}, {1}, 0, read_synth_locked },
    { "board.uptime", "Seconds since boot", "seconds",
      SENSOR_INTEGER, {0}, {INT64_MAX}, 0, read_uptime_s },
    { "mcu.temperature", "STM32F407 die temperature (uncalibrated ADC estimate)", "degrees_C",
      SENSOR_FLOAT, {.f = -40.0}, {.f = 125.0}, 3, read_mcu_temperature },
    PAC1954_SENSORS(1),
    PAC1954_SENSORS(2),
    PAC1954_SENSORS(3),
    PAC1954_SENSORS(4),
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
        snprintf(minbuf, sizeof(minbuf), "%lld", (long long)s->min.i);
        snprintf(maxbuf, sizeof(maxbuf), "%lld", (long long)s->max.i);
        const char *info[] = { s->name, s->description, s->units,
                                type_name(s->type), minbuf, maxbuf };
        katcp_inform(ctx, "sensor-list", 6, info);
    } else if (s->type == SENSOR_FLOAT) {
        char minbuf[32], maxbuf[32];
        snprintf(minbuf, sizeof(minbuf), "%.*f", s->decimals, s->min.f);
        snprintf(maxbuf, sizeof(maxbuf), "%.*f", s->decimals, s->max.f);
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
    sensor_value_t v = {0};
    sensor_status_t st = s->read(&v);

    /* KATCP v5 specifies sensor timestamps as UTC seconds, not device
     * uptime (see IMPLEMENTATION.md section 7 for the full discussion).
     * On a target with no synced wall clock (or before the first sync
     * completes), platform_wallclock_unix_s() returns 0 -- an honest
     * "unsynced" signal (1970-01-01) rather than a plausible-looking
     * but meaningless boot-relative number. */
    char tsbuf[24], valbuf[32];
    snprintf(tsbuf, sizeof(tsbuf), "%lu", (unsigned long)platform_wallclock_unix_s());
    if (s->type == SENSOR_BOOLEAN)
        snprintf(valbuf, sizeof(valbuf), "%d", v.i ? 1 : 0);
    else if (s->type == SENSOR_FLOAT)
        snprintf(valbuf, sizeof(valbuf), "%.*f", s->decimals, v.f);
    else
        snprintf(valbuf, sizeof(valbuf), "%lld", (long long)v.i);

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
