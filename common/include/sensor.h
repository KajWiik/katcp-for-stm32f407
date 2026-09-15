#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>
#include "katcp_proto.h"

/* Wire-level values stay integer or boolean -- SENSOR_FLOAT below is
 * still represented internally as a scaled integer (see `scale` on
 * sensor_def_t) and formatted by hand as fixed-point decimal text; it
 * is never a C `float`/`double` and never goes through `%f`. See
 * IMPLEMENTATION.md §6.1/§6.2 for why: not an FPU-availability issue,
 * it's about %f/newlib formatting reliability and keeping arithmetic
 * simple and deterministic. */
typedef enum {
    SENSOR_BOOLEAN,
    SENSOR_INTEGER,
    SENSOR_FLOAT,   /* KATCP "float" type; scaled-integer internally, see above */
} sensor_type_t;

typedef enum {
    SENSOR_STATUS_UNKNOWN,
    SENSOR_STATUS_NOMINAL,
    SENSOR_STATUS_WARN,
    SENSOR_STATUS_ERROR,
} sensor_status_t;

/* Reads the current value into *out_value (0/1 for SENSOR_BOOLEAN;
 * for SENSOR_FLOAT, the value scaled by `scale` below -- e.g.
 * millidegrees for a scale of 1000). Must not block. Returns the
 * sensor's current status. */
typedef sensor_status_t (*sensor_read_fn)(int64_t *out_value);

typedef struct {
    const char     *name;         /* e.g. "synth.locked" */
    const char     *description;  /* e.g. "Synthesizer PLL lock status" */
    const char     *units;        /* "" if dimensionless */
    sensor_type_t   type;
    int64_t         min, max;     /* meaningful for SENSOR_INTEGER and SENSOR_FLOAT (raw/scaled units) */
    int64_t         scale;        /* SENSOR_FLOAT only: raw units per 1.0 of the displayed
                                      value, e.g. 1000 for millidegrees -> "23.456". Must be
                                      a positive power of 10. Unused (0) for other types. */
    sensor_read_fn  read;
} sensor_def_t;

extern const sensor_def_t sensor_table[];
extern const int sensor_table_len;

/* Handlers -- add these two rows to katcp_command_table in
 * katcp_commands.c to register them (already done). */
void cmd_sensor_list(katcp_ctx_t *ctx, int argc, char **argv, void *user);
void cmd_sensor_value(katcp_ctx_t *ctx, int argc, char **argv, void *user);

#endif /* SENSOR_H */
