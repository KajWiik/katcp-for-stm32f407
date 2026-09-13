#include "katcp_commands.h"
#include "synth.h"
#include "sensor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *DEVICE_NAME    = "synth-katcp-bridge";
const char *DEVICE_VERSION = "0.1";

volatile int katcp_halt_requested = 0;

/* ---- ?halt ------------------------------------------------------------ */
static void cmd_halt(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)argc; (void)argv; (void)user;
    katcp_reply(ctx, "ok", 0, NULL);
    katcp_halt_requested = 1; /* main.c decides what "halt" means on this box */
}

/* ---- ?watchdog ---------------------------------------------------------- */
static void cmd_watchdog(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)argc; (void)argv; (void)user;
    katcp_reply(ctx, "ok", 0, NULL);
}

/* ---- ?version-list ------------------------------------------------------ */
static void cmd_version_list(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)argc; (void)argv; (void)user;
    const char *info[] = { DEVICE_NAME, DEVICE_VERSION };
    katcp_inform(ctx, "version-list", 2, info);
    const char *n[] = { "1" };
    katcp_reply(ctx, "ok", 1, n);
}

/* ---- ?synth-frequency <hz> ----------------------------------------------
 * Sets the synthesizer output frequency. Custom (non-core) request --
 * name deliberately avoids clashing with any core KATCP request. */
static void cmd_synth_frequency(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)user;
    if (argc != 2) {
        const char *msg[] = { "usage: ?synth-frequency <hz>" };
        katcp_reply(ctx, "invalid", 1, msg);
        return;
    }

    char *end;
    uint64_t hz = strtoull(argv[1], &end, 10);
    if (*end != '\0') {
        const char *msg[] = { "frequency must be an integer in Hz" };
        katcp_reply(ctx, "invalid", 1, msg);
        return;
    }

    if (synth_set_frequency(hz)) {
        katcp_reply(ctx, "ok", 0, NULL);
    } else {
        const char *msg[] = { "synth programming failed or out of range" };
        katcp_reply(ctx, "fail", 1, msg);
    }
}

/* ---- ?synth-locked ------------------------------------------------------- */
static void cmd_synth_locked(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)argc; (void)argv; (void)user;
    const char *locked[] = { synth_is_locked() ? "1" : "0" };
    katcp_reply(ctx, "ok", 1, locked);
}

/* ---- ?help --------------------------------------------------------------
 * Defined last so it can walk katcp_command_table (needs the table to
 * already be declared -- see the extern declaration + table below). */
static void cmd_help(katcp_ctx_t *ctx, int argc, char **argv, void *user);

const katcp_cmd_t katcp_command_table[] = {
    { "halt",             "Halt the device.",                         cmd_halt },
    { "watchdog",         "Check the server is alive.",                cmd_watchdog },
    { "version-list",     "List component name and version.",         cmd_version_list },
    { "help",             "Describe available requests.",              cmd_help },
    { "synth-frequency",  "Set synth output frequency in Hz.",         cmd_synth_frequency },
    { "synth-locked",     "Report synth PLL lock status (0/1).",       cmd_synth_locked },
    { "sensor-list",      "List available sensors, or describe one.", cmd_sensor_list },
    { "sensor-value",     "Report current sensor value(s).",          cmd_sensor_value },
};
const int katcp_command_table_len =
    sizeof(katcp_command_table) / sizeof(katcp_command_table[0]);

static void cmd_help(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)user;
    if (argc == 1) {
        for (int i = 0; i < katcp_command_table_len; i++) {
            const char *info[] = { katcp_command_table[i].name,
                                    katcp_command_table[i].help };
            katcp_inform(ctx, "help", 2, info);
        }
        char count[8];
        snprintf(count, sizeof(count), "%d", katcp_command_table_len);
        const char *n[] = { count };
        katcp_reply(ctx, "ok", 1, n);
        return;
    }

    for (int i = 0; i < katcp_command_table_len; i++) {
        if (strcmp(katcp_command_table[i].name, argv[1]) == 0) {
            const char *info[] = { katcp_command_table[i].name,
                                    katcp_command_table[i].help };
            katcp_inform(ctx, "help", 2, info);
            const char *n[] = { "1" };
            katcp_reply(ctx, "ok", 1, n);
            return;
        }
    }
    const char *msg[] = { "unknown request" };
    katcp_reply(ctx, "fail", 1, msg);
}
