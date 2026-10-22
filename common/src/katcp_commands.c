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

/* ---- synth selector -------------------------------------------------------
 * Every synth-* request takes the synth number (1..SYNTH_COUNT, as
 * users count them) as its first argument. Returns the 0-based index
 * synth.h wants, or -1 after replying "invalid" itself. */
static int parse_synth_arg(katcp_ctx_t *ctx, const char *arg)
{
    char *end;
    long n = strtol(arg, &end, 10);
    if (*end != '\0' || n < 1 || n > SYNTH_COUNT) {
        char msg_buf[48];
        snprintf(msg_buf, sizeof(msg_buf), "synth must be 1-%d", SYNTH_COUNT);
        const char *msg[] = { msg_buf };
        katcp_reply(ctx, "invalid", 1, msg);
        return -1;
    }
    return (int)n - 1;
}

/* ---- ?synth-frequency <synth> <hz> ---------------------------------------
 * Sets one synthesizer's output frequency. Custom (non-core) request --
 * name deliberately avoids clashing with any core KATCP request. */
static void cmd_synth_frequency(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)user;
    if (argc != 3) {
        const char *msg[] = { "usage: ?synth-frequency <synth> <hz>" };
        katcp_reply(ctx, "invalid", 1, msg);
        return;
    }

    int synth = parse_synth_arg(ctx, argv[1]);
    if (synth < 0)
        return;

    char *end;
    uint64_t hz = strtoull(argv[2], &end, 10);
    if (*end != '\0') {
        const char *msg[] = { "frequency must be an integer in Hz" };
        katcp_reply(ctx, "invalid", 1, msg);
        return;
    }

    if (synth_set_frequency(synth, hz)) {
        katcp_reply(ctx, "ok", 0, NULL);
    } else {
        const char *msg[] = { "synth programming failed or out of range" };
        katcp_reply(ctx, "fail", 1, msg);
    }
}

/* ---- ?synth-locked <synth> ---------------------------------------------- */
static void cmd_synth_locked(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)user;
    if (argc != 2) {
        const char *msg[] = { "usage: ?synth-locked <synth>" };
        katcp_reply(ctx, "invalid", 1, msg);
        return;
    }
    int synth = parse_synth_arg(ctx, argv[1]);
    if (synth < 0)
        return;
    const char *locked[] = { synth_is_locked(synth) ? "1" : "0" };
    katcp_reply(ctx, "ok", 1, locked);
}

/* ---- ?synth-write-register <synth> <word24> ------------------------------
 * Writes one raw 24-bit LMX2594-style register word verbatim -- the
 * one-request-per-register path for replaying a precomputed register
 * dump (see synth_write_raw_register() in synth.h for what this
 * deliberately does NOT do: no frequency math, no charge-pump/VCO-
 * curve awareness). Accepts either hex (0x...) or decimal via base-0
 * strtoul, matching the register dump files' own "0x700000" notation. */
static void cmd_synth_write_register(katcp_ctx_t *ctx, int argc, char **argv, void *user)
{
    (void)user;
    if (argc != 3) {
        const char *msg[] = { "usage: ?synth-write-register <synth> <24bit-value>" };
        katcp_reply(ctx, "invalid", 1, msg);
        return;
    }

    int synth = parse_synth_arg(ctx, argv[1]);
    if (synth < 0)
        return;

    char *end;
    unsigned long val = strtoul(argv[2], &end, 0);
    if (*end != '\0' || val > 0xFFFFFFUL) {
        const char *msg[] = { "value must be a 24-bit integer (0x000000-0xFFFFFF)" };
        katcp_reply(ctx, "invalid", 1, msg);
        return;
    }

    if (synth_write_raw_register(synth, (uint32_t)val)) {
        katcp_reply(ctx, "ok", 0, NULL);
    } else {
        const char *msg[] = { "synth register write failed" };
        katcp_reply(ctx, "fail", 1, msg);
    }
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
    { "synth-frequency",  "Set synth <n> output frequency in Hz.",     cmd_synth_frequency },
    { "synth-locked",     "Report synth <n> PLL lock status (0/1).",   cmd_synth_locked },
    { "synth-write-register", "Write one raw 24-bit word to synth <n>.", cmd_synth_write_register },
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
