/*
 * KATCP server for the Olimex STM32-E407 (Rev E+, LAN8710A PHY),
 * using ChibiOS + lwIP against the board's native Ethernet MAC --
 * no W5500 or other SPI Ethernet chip involved on this target.
 *
 * Board pin/PHY configuration comes entirely from ChibiOS's own
 * OLIMEX_STM32_E407_REV_D board files (see Makefile) -- this file
 * only wires lwIP's socket API to the shared katcp_proto/katcp_commands
 * layer in ../../common, the same way targets/rp2040-w5500-pico/src/main.c
 * wires the WIZnet socket API to it.
 */

#include <string.h>

#include "ch.h"
#include "hal.h"

#include "lwipthread.h"
#include "lwip/sockets.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"

#include "katcp_proto.h"
#include "katcp_commands.h"
#include "synth.h"
#include "platform.h"
#include "portab.h"

#define KATCP_PORT 7147

/* ---- network config -------------------------------------------------------
 * Same two-stage policy as the RP2040 target (IMPLEMENTATION.md \u00a75.2):
 * try DHCP for up to DHCP_TIMEOUT_S seconds, then fall back to this
 * static config. Change per deployment; give each unit a distinct MAC. */
static uint8_t mac_address[6] = {0xC2, 0xAF, 0x51, 0x03, 0xCF, 0x01};

#ifndef DHCP_TIMEOUT_S
#define DHCP_TIMEOUT_S 30
#endif

static void link_up_cb(void *p)   { (void)p; }
static void link_down_cb(void *p) { (void)p; }

/*
 * Unlike the RP2040 target's dhcp_bringup() (which drives
 * ioLibrary_Driver's DHCP state machine directly), lwipInit() starts
 * DHCP as a side effect of NET_ADDRESS_DHCP and returns immediately --
 * the actual lease negotiation happens asynchronously in lwIP's tcpip
 * thread. So the "wait up to N seconds" loop here just polls
 * dhcp_supplied_address() rather than driving a state machine itself.
 * On timeout, lwipReconfigure() to NET_ADDRESS_STATIC does the actual
 * "stop DHCP, apply static values" work (see lwipthread.c's
 * do_reconfigure()) -- there's no separate fallback path to write,
 * same as the RP2040 target's "not overwriting net_info is the
 * fallback" logic.
 */
static void network_bringup(void)
{
    lwipthread_opts_t opts;
    memset(&opts, 0, sizeof(opts));

    opts.macaddress   = mac_address;
    opts.address      = IP4_ADDR_VALUE(192, 168, 1, 200);
    opts.netmask      = IP4_ADDR_VALUE(255, 255, 255, 0);
    opts.gateway      = IP4_ADDR_VALUE(192, 168, 1, 1);
    opts.addrMode     = NET_ADDRESS_DHCP;
    opts.link_up_cb   = link_up_cb;
    opts.link_down_cb = link_down_cb;

    lwipInit(&opts);

    uint64_t start_ms = platform_now_ms();
    while (!dhcp_supplied_address(netif_default)) {
        if (platform_now_ms() - start_ms >= (uint64_t)DHCP_TIMEOUT_S * 1000) {
            lwipreconf_opts_t reconf = {
                .address  = opts.address,
                .netmask  = opts.netmask,
                .gateway  = opts.gateway,
                .addrMode = NET_ADDRESS_STATIC,
            };
            lwipReconfigure(&reconf);
            return;
        }
        chThdSleepMilliseconds(100);
    }
}

/* ---- katcp_out callback: parser hands us bytes, we send() them ---------- */
static void katcp_out_to_socket(void *user, const char *data, size_t len)
{
    int sock = *(int *)user;
    send(sock, data, len, 0);
}

/*
 * One client at a time, same as the RP2040 target (see
 * IMPLEMENTATION.md \u00a75.5) -- this is a shape difference from that
 * target's single polling superloop, though: lwIP's blocking socket
 * calls are the natural fit for a ChibiOS thread, so the KATCP server
 * runs as its own thread here rather than a state machine polled from
 * main(). common/'s katcp_proto.c and katcp_commands.c don't care
 * either way -- they only see katcp_feed() calls and an output
 * callback, identically on both targets.
 */
static THD_WORKING_AREA(wa_katcp_thread, 2048);
static THD_FUNCTION(katcp_thread, arg)
{
    (void)arg;
    chRegSetThreadName("katcp");

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(KATCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_sock, 1);

    for (;;) {
        int client = accept(listen_sock, NULL, NULL);
        if (client < 0) {
            continue;
        }

        katcp_ctx_t ctx;
        katcp_init(&ctx, katcp_command_table, katcp_command_table_len,
                   katcp_out_to_socket, &client);

        char rx_scratch[512];
        for (;;) {
            int got = recv(client, rx_scratch, sizeof(rx_scratch), 0);
            if (got <= 0) {
                break; /* client closed, or a socket error */
            }
            katcp_feed(&ctx, rx_scratch, (size_t)got);
            if (katcp_halt_requested) {
                /* Same open question as the RP2040 target: "halt"
                 * doesn't yet have a project-specific meaning beyond
                 * dropping the connection. See IMPLEMENTATION.md \u00a74. */
                katcp_halt_requested = 0;
                break;
            }
        }
        close(client);
    }
}

int main(void)
{
    halInit();
    chSysInit();

    network_bringup();
    synth_init();
    portab_setup();

    chThdCreateStatic(wa_katcp_thread, sizeof(wa_katcp_thread),
                       NORMALPRIO, katcp_thread, NULL);

    /* Heartbeat -- main() becomes the idle-priority loop once its
     * setup work is done, same role the RP2040 target's superloop
     * plays, just visibly blinking here instead of polling a socket. */
    while (true) {
        palToggleLine(PORTAB_LINE_LED1);
        chThdSleepMilliseconds(500);
    }
}
