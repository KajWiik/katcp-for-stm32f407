#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>
#include "katcp_proto.h"

/* SENSOR_FLOAT uses a genuine C `double` -- this project used to
 * avoid float/double everywhere (scaled integers + hand-rolled
 * fixed-point text formatting instead) mainly over concern that
 * printf's `%f` isn't reliably linked in by newlib-nano. That
 * concern doesn't apply to this project's actual build: it doesn't
 * use `--specs=nano.specs` anywhere (checked directly in the
 * Makefile/ChibiOS build rules, not assumed), so full newlib with
 * working `%f` is what's actually linked. Combined with the synth
 * loop-setup math (IMPLEMENTATION.md §6.3) genuinely benefiting from
 * real floating point, the FPU is now enabled (targets/stm32-e407/
 * Makefile, USE_FPU=hard) and SENSOR_FLOAT reads/reports real
 * doubles, formatted via `%.*f`. See IMPLEMENTATION.md §6.1 for the
 * full history of this reversal -- it's a reversal, not something
 * pretended to have been the design from the start. */
typedef enum {
    SENSOR_BOOLEAN,
    SENSOR_INTEGER,
    SENSOR_FLOAT,
} sensor_type_t;

typedef enum {
    SENSOR_STATUS_UNKNOWN,
    SENSOR_STATUS_NOMINAL,
    SENSOR_STATUS_WARN,
    SENSOR_STATUS_ERROR,
} sensor_status_t;

/* Which union member a sensor_read_fn fills in is determined by the
 * sensor_def_t's `type` -- SENSOR_BOOLEAN/SENSOR_INTEGER use `.i`,
 * SENSOR_FLOAT uses `.f`. */
typedef union {
    int64_t i;
    double  f;
} sensor_value_t;

/* Fills in *out (the `.i` or `.f` member matching the sensor's own
 * type -- see sensor_value_t above). Must not block. Returns the
 * sensor's current status. */
typedef sensor_status_t (*sensor_read_fn)(sensor_value_t *out);

typedef struct {
    const char     *name;         /* e.g. "synth.locked" */
    const char     *description;  /* e.g. "Synthesizer PLL lock status" */
    const char     *units;        /* "" if dimensionless */
    sensor_type_t   type;
    sensor_value_t  min, max;     /* meaningful for SENSOR_INTEGER (.i) and SENSOR_FLOAT (.f); unused for SENSOR_BOOLEAN */
    int             decimals;     /* SENSOR_FLOAT only: decimal places for %.*f formatting
                                      of both the value and min/max, e.g. 3 -> "23.456".
                                      Unused for other types. */
    sensor_read_fn  read;
} sensor_def_t;

extern const sensor_def_t sensor_table[];
extern const int sensor_table_len;

/* Handlers -- add these two rows to katcp_command_table in
 * katcp_commands.c to register them (already done). */
void cmd_sensor_list(katcp_ctx_t *ctx, int argc, char **argv, void *user);
void cmd_sensor_value(katcp_ctx_t *ctx, int argc, char **argv, void *user);

#endif /* SENSOR_H */
