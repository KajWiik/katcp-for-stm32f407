#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>
#include "katcp_proto.h"

/* Deliberately integer/boolean only -- see IMPLEMENTATION.md §6.1 for
 * why floating point is avoided in this project's wire-facing code
 * (not an FPU-availability issue -- it's about %f formatting/newlib
 * reliability and keeping things simple and deterministic). If a
 * genuinely fractional sensor is ever needed, prefer a scaled integer
 * (e.g. millidegrees) over adding a float type here. */
typedef enum {
    SENSOR_BOOLEAN,
    SENSOR_INTEGER,
} sensor_type_t;

typedef enum {
    SENSOR_STATUS_UNKNOWN,
    SENSOR_STATUS_NOMINAL,
    SENSOR_STATUS_WARN,
    SENSOR_STATUS_ERROR,
} sensor_status_t;

/* Reads the current value into *out_value (0/1 for SENSOR_BOOLEAN).
 * Must not block. Returns the sensor's current status. */
typedef sensor_status_t (*sensor_read_fn)(int64_t *out_value);

typedef struct {
    const char     *name;         /* e.g. "synth.locked" */
    const char     *description;  /* e.g. "Synthesizer PLL lock status" */
    const char     *units;        /* "" if dimensionless */
    sensor_type_t   type;
    int64_t         min, max;     /* only meaningful for SENSOR_INTEGER */
    sensor_read_fn  read;
} sensor_def_t;

extern const sensor_def_t sensor_table[];
extern const int sensor_table_len;

/* Handlers -- add these two rows to katcp_command_table in
 * katcp_commands.c to register them (already done). */
void cmd_sensor_list(katcp_ctx_t *ctx, int argc, char **argv, void *user);
void cmd_sensor_value(katcp_ctx_t *ctx, int argc, char **argv, void *user);

#endif /* SENSOR_H */
