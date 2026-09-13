/*
 * Wires the transport-agnostic KATCP parser (katcp_proto.c) to a W5500
 * TCP socket via WIZnet's ioLibrary_Driver, on a W5500-EVB-Pico.
 *
 * This file assumes ioLibrary_Driver is vendored/submoduled at
 * ./ioLibrary_Driver as in WIZnet's own RP2040-HAT-C examples
 * (https://github.com/Wiznet/RP2040-HAT-C) -- see CMakeLists.txt.
 *
 * Pin assignment matches the W5500-EVB-Pico's on-board wiring, which is
 * fixed by the board (not a free choice): SPI0 on GPIO16-19, plus reset
 * and interrupt on GPIO20/21.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "wizchip_conf.h"
#include "w5500.h"
#include "socket.h"
#include "dhcp.h"

#include "katcp_proto.h"
#include "katcp_commands.h"
#include "synth.h"
#include "platform.h"

/* ---- W5500-EVB-Pico fixed pinout (SPI0) --------------------------------- */
#define PIN_MISO  16
#define PIN_CS    17
#define PIN_SCK   18
#define PIN_MOSI  19
#define PIN_RST   20
#define PIN_INT   21   /* not used here; polling getSn_SR() instead */

#define SPI_PORT  spi0

/* ---- network config -------------------------------------------------------
 * This is both the fallback config (used as-is if DHCP times out, see
 * dhcp_bringup() below) and the baseline the chip is programmed with
 * *before* DHCP is attempted, so the device always has some valid
 * config from the moment wizchip_init() returns.
 *
 * TODO: replace ip/sn/gw/dns with your site's static addressing, and
 * give each unit a distinct MAC (the locally-administered
 * 02:00:00:00:00:01 placeholder below must be unique per box on the
 * same segment, DHCP or not -- DHCP servers key leases on it too). */
static wiz_NetInfo net_info = {
    .mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}, /* locally-administered MAC -- change per unit */
    .ip  = {192, 168, 1, 200},
    .sn  = {255, 255, 255, 0},
    .gw  = {192, 168, 1, 1},
    .dns = {192, 168, 1, 1},
    .dhcp = NETINFO_STATIC,
};

/* Seconds to wait for a DHCP lease at boot before giving up and running
 * on the static net_info above instead. Override with
 * -DDHCP_TIMEOUT_S=<n> at build time if 30 isn't right for your site. */
#ifndef DHCP_TIMEOUT_S
#define DHCP_TIMEOUT_S 30
#endif

#define DHCP_SOCKET  7   /* kept separate from KATCP_SOCKET (0) below */

#define KATCP_SOCKET   0
#define KATCP_PORT     7147   /* conventional KATCP/tcpborphserver port */

static uint8_t rx_scratch[512];

/* ---- SPI glue required by wizchip_conf.h callbacks ----------------------- */
static void w5500_cs_select(void)   { gpio_put(PIN_CS, 0); }
static void w5500_cs_deselect(void) { gpio_put(PIN_CS, 1); }

static uint8_t w5500_spi_read_byte(void)
{
    uint8_t rx = 0, tx = 0xFF;
    spi_write_read_blocking(SPI_PORT, &tx, &rx, 1);
    return rx;
}
static void w5500_spi_write_byte(uint8_t b)
{
    spi_write_blocking(SPI_PORT, &b, 1);
}
static void w5500_spi_read_burst(uint8_t *buf, uint16_t len)
{
    uint8_t tx = 0xFF;
    for (uint16_t i = 0; i < len; i++)
        spi_write_read_blocking(SPI_PORT, &tx, &buf[i], 1);
}
static void w5500_spi_write_burst(uint8_t *buf, uint16_t len)
{
    spi_write_blocking(SPI_PORT, buf, len);
}

static void w5500_hw_reset(void)
{
    gpio_put(PIN_RST, 0);
    sleep_ms(10);
    gpio_put(PIN_RST, 1);
    sleep_ms(50); /* datasheet: allow PLL lock time after reset */
}

static void spi_and_wizchip_init(void)
{
    spi_init(SPI_PORT, 20 * 1000 * 1000); /* 20 MHz -- W5500 supports up to ~80 MHz, start conservative */
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    w5500_hw_reset();

    reg_wizchip_cs_cbfunc(w5500_cs_select, w5500_cs_deselect);
    reg_wizchip_spi_cbfunc(w5500_spi_read_byte, w5500_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(w5500_spi_read_burst, w5500_spi_write_burst);

    /* 2KB TX/2KB RX per socket x8 sockets = the W5500's full 16KB/16KB split.
     * We only use socket 0, but ioLibrary_Driver wants sizes for all 8. */
    uint8_t tx_sizes[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    uint8_t rx_sizes[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    wizchip_init(tx_sizes, rx_sizes);

    wizchip_setnetinfo(&net_info);
}

/* ---- DHCP bring-up (one-shot, blocking, at startup only) -----------------
 *
 * Tries for up to DHCP_TIMEOUT_S seconds to get a lease. On success,
 * net_info and the chip's own registers are updated to the leased
 * address. On timeout or failure, net_info is left exactly as
 * spi_and_wizchip_init() already programmed it -- there is nothing
 * further to "fall back" to here, since the static config was never
 * overwritten in the first place.
 *
 * This does NOT keep retrying in the background after this call
 * returns. If no DHCP server is present at boot, the device runs on
 * the static config for the rest of its uptime, even if a server
 * later appears. That's a deliberate scope decision (see
 * IMPLEMENTATION.md) -- extend this if you need it to notice a
 * server that shows up later, e.g. by calling dhcp_bringup() again
 * from a `?net-renew`-style command. */
static uint8_t dhcp_buffer[1024];
static volatile bool dhcp_got_ip = false;

static void dhcp_ip_assigned(void) { dhcp_got_ip = true; }
static void dhcp_ip_updated(void)  { dhcp_got_ip = true; }
static void dhcp_ip_conflict(void)
{
    /* Leased address conflicts with something already on the network.
     * We don't have a good automated recovery for this beyond falling
     * back to the static config, which the timeout path below already
     * does -- so just stop trying rather than loop on a bad lease. */
    dhcp_got_ip = false;
}

static void dhcp_bringup(void)
{
    reg_dhcp_cbfunc(dhcp_ip_assigned, dhcp_ip_updated, dhcp_ip_conflict);
    DHCP_init(DHCP_SOCKET, dhcp_buffer);

    uint64_t start_ms     = platform_now_ms();
    uint64_t last_tick_ms = start_ms;

    while (!dhcp_got_ip) {
        uint64_t now = platform_now_ms();

        if (now - start_ms >= (uint64_t)DHCP_TIMEOUT_S * 1000) {
            printf("DHCP: no lease after %ds, using static %d.%d.%d.%d\n",
                   DHCP_TIMEOUT_S, net_info.ip[0], net_info.ip[1],
                   net_info.ip[2], net_info.ip[3]);
            DHCP_stop();
            return;
        }

        /* DHCP's internal lease/retry timers are driven off wall-clock
         * seconds, not loop iterations -- must be ticked at ~1 Hz
         * regardless of how often DHCP_run() itself is called. */
        if (now - last_tick_ms >= 1000) {
            DHCP_time_handler();
            last_tick_ms = now;
        }

        if (DHCP_run() == DHCP_FAILED) {
            printf("DHCP: failed, using static %d.%d.%d.%d\n",
                   net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
            DHCP_stop();
            return;
        }
    }

    getIPfromDHCP(net_info.ip);
    getGWfromDHCP(net_info.gw);
    getSNfromDHCP(net_info.sn);
    getDNSfromDHCP(net_info.dns);
    net_info.dhcp = NETINFO_DHCP;
    wizchip_setnetinfo(&net_info);
    printf("DHCP: leased %d.%d.%d.%d\n",
           net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
}

/* ---- katcp_out callback: parser hands us bytes, we send() them ---------- */
static void katcp_out_to_socket(void *user, const char *data, size_t len)
{
    int sock = *(int *)user;
    send(sock, (uint8_t *)data, (uint16_t)len);
}

int main(void)
{
    stdio_init_all(); /* USB serial, for printf() debug only -- KATCP itself is on Ethernet */

    spi_and_wizchip_init();
    dhcp_bringup();
    synth_init();

    int sock = KATCP_SOCKET;
    katcp_ctx_t ctx;
    katcp_init(&ctx, katcp_command_table, katcp_command_table_len,
               katcp_out_to_socket, &sock);

    for (;;) {
        uint8_t status = getSn_SR(sock);

        switch (status) {
            case SOCK_CLOSED:
                socket(sock, Sn_MR_TCP, KATCP_PORT, 0x00);
                break;

            case SOCK_INIT:
                listen(sock);
                break;

            case SOCK_ESTABLISHED:
                if (getSn_IR(sock) & Sn_IR_CON) {
                    setSn_IR(sock, Sn_IR_CON);
                }
                {
                    uint16_t avail = getSn_RX_RSR(sock);
                    if (avail > 0) {
                        if (avail > sizeof(rx_scratch)) avail = sizeof(rx_scratch);
                        int32_t got = recv(sock, rx_scratch, avail);
                        if (got > 0) {
                            katcp_feed(&ctx, (char *)rx_scratch, (size_t)got);
                        }
                    }
                }
                if (katcp_halt_requested) {
                    /* Project-specific: decide what "halt" means on a
                     * dedicated synth box. For now, just drop the
                     * connection; a real implementation might also
                     * park the synth in a safe state before resetting. */
                    disconnect(sock);
                    katcp_halt_requested = 0;
                }
                break;

            case SOCK_CLOSE_WAIT:
                disconnect(sock);
                break;

            default:
                break;
        }
    }
}
