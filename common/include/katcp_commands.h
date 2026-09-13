#ifndef KATCP_COMMANDS_H
#define KATCP_COMMANDS_H

#include "katcp_proto.h"

/* Device identity strings used by ?version-list. Define these in your
 * main.c (or wherever you keep board config) before building. */
extern const char *DEVICE_NAME;
extern const char *DEVICE_VERSION;

/* The command table and its length -- pass straight to katcp_init(). */
extern const katcp_cmd_t katcp_command_table[];
extern const int katcp_command_table_len;

/* Set to 1 by the ?halt handler; main.c should check this after each
 * katcp_feed() call and, if set, cleanly close the socket / reset. */
extern volatile int katcp_halt_requested;

#endif /* KATCP_COMMANDS_H */
