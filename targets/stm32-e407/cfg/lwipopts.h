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
 * this to 0. See IMPLEMENTATION.md section 5.2 for why DHCP-then-
 * static-fallback is the policy here, matching the RP2040 target. */
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

#endif /* LWIP_HDR_LWIPOPTS_H__ */
