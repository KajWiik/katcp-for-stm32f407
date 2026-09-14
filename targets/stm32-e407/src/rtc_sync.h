#ifndef RTC_SYNC_H
#define RTC_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "lwip/ip4_addr.h"

/*
 * SNTP-based RTC time sync. See IMPLEMENTATION.md section 7 for the
 * design (boot sync + periodic resync via lwIP's own SNTP module and
 * its SNTP_UPDATE_DELAY -- no polling loop of our own is needed; see
 * cfg/lwipopts.h for that wiring). No battery backup is required
 * since the time is always re-fetched from the network rather than
 * relied on to survive a power cycle.
 */

/* Starts SNTP sync against server_ip (this site's maser/GNSS-
 * disciplined local time server -- never a public pool, see
 * IMPLEMENTATION.md). Call only after the network is up, i.e. after
 * network_bringup() in main.c. Fires one request shortly after this
 * call, then automatically every 6 hours from then on. */
void rtc_sync_start(const ip4_addr_t *server_ip);

/* Current wall-clock time as Unix epoch seconds (UTC), read back from
 * the RTC. Meaningless (whatever the RTC happened to power up or last
 * be set to) until rtc_sync_is_synced() is true. */
uint32_t rtc_sync_unix_time(void);

/* True once at least one SNTP sync has successfully set the RTC since
 * boot. Does not by itself tell you how stale that sync is -- that's
 * a natural follow-on (e.g. a sensor exposing seconds-since-last-sync)
 * if it turns out to matter, not built here. */
bool rtc_sync_is_synced(void);

#endif /* RTC_SYNC_H */
