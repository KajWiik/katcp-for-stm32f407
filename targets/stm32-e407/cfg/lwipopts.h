/*
 * lwIP options for the STM32-E407 KATCP target.
 *
 * Most settings come from ChibiOS's static_lwipopts.h (mandatory,
 * fixed by the ChibiOS/lwIP integration). The overrides below are
 * this project's own choices, not stock demo leftovers.
 */
#ifndef LWIP_HDR_LWIPOPTS_H__
#define LWIP_HDR_LWIPOPTS_H__

/* Fixed settings mandated by the ChibiOS integration. */
#include "static_lwipopts.h"

/* lwipthread.h's NET_ADDRESS_DHCP mode (used by network_bringup() in
 * main.c) only exists when LWIP_DHCP is set -- vanilla lwIP defaults
 * this to 0. See IMPLEMENTATION.md section 5.1 for the DHCP-then-
 * static-fallback policy. */
#define LWIP_DHCP                        1

/* Default (1600 bytes) is tight once DHCP, one TCP connection, and
 * their buffers are all live at once. The F407 has 192KB SRAM to
 * spare, so there's no reason to run close to the edge. */
#define MEM_SIZE                         8192

#if !defined(TCPIP_MBOX_SIZE)
#define TCPIP_MBOX_SIZE                  MEMP_NUM_PBUF
#endif
#if !defined(TCPIP_THREAD_STACKSIZE)
#define TCPIP_THREAD_STACKSIZE           1024
#endif
#if !defined(TCPIP_THREAD_PRIO)
#define TCPIP_THREAD_PRIO                (LOWPRIO + 1)
#endif
#if !defined(LWIP_THREAD_PRIORITY)
#define LWIP_THREAD_PRIORITY             (LOWPRIO)
#endif

/* ---- SNTP-based RTC sync (IMPLEMENTATION.md §7: boot sync + 6h
 * resync, no battery backup needed since it's re-fetched from
 * network every time). The actual conversion/rtcSetTime() work is in
 * src/rtc_sync.c; this only wires lwIP's SNTP module to call it. */

/* Forward declaration -- src/rtc_sync.c provides the definition.
 * Kept here rather than a separate header since this macro is the
 * only thing that needs it visible to sntp.c's compilation unit. */
#include <stdint.h>
void rtc_sync_on_sntp_time(uint32_t unix_time);
#define SNTP_SET_SYSTEM_TIME(sec)  rtc_sync_on_sntp_time((uint32_t)(sec))

/* Local, statically-configured server (this site's maser/GNSS-
 * disciplined time reference -- see src/main.c), not a public pool,
 * so no DNS resolution needed. */
#define SNTP_SERVER_DNS             0

/* The RFC's 1-5 minute random startup delay exists to avoid hammering
 * public NTP pools; irrelevant for a dedicated local server. */
#define SNTP_STARTUP_DELAY          0

/* Resync every 6 hours (spec minimum is 60000 ms). */
#define SNTP_UPDATE_DELAY           (6UL * 60UL * 60UL * 1000UL)

#endif /* LWIP_HDR_LWIPOPTS_H__ */
