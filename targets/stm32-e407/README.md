# stm32-e407

KATCP server for the Olimex STM32-E407 (Rev E or later — LAN8710A
PHY), using ChibiOS + lwIP against the board's native Ethernet MAC.
Unlike `../rp2040-w5500-pico`, there is no W5500 or other SPI Ethernet
chip here at all — the MAC is on-chip and lwIP talks to it directly.
GNU Make + ChibiOS's own build conventions, not CMake.

## Layout

```
src/
  main.c              -- ChibiOS/lwIP glue: network bring-up, KATCP-over-socket thread
  platform_chibios.c  -- platform_now_ms() via ChibiOS's chVTGetSystemTimeX()
  portab.c / portab.h -- minimal board hook (LED heartbeat), ChibiOS demo convention
cfg/
  chconf.h            -- RTOS kernel config (stock ChibiOS template, unmodified)
  halconf.h           -- HAL subsystem config (PAL + MAC enabled; USB/SDC/shell dropped)
  mcuconf.h           -- STM32F4xx peripheral/clock config (stock, USB disabled)
  lwipopts.h          -- this project's lwIP overrides (DHCP enabled, larger heap)
Makefile
```

The protocol, command table, sensor table, and synth driver contract
this target builds against live in `../../common/include` and
`../../common/src` — identical to `../rp2040-w5500-pico`, and the
Makefile pulls them in automatically (`AUTOBUILD_ROOT`). See
`../../IMPLEMENTATION.md` for how all of that fits together and the
full command reference.

**One real architectural difference from the RP2040 target, worth
knowing going in:** that target polls a single socket from a bare
superloop. Here, lwIP's blocking BSD-socket calls are the natural fit
for a ChibiOS thread, so the KATCP server runs as its own thread
(`katcp_thread` in `main.c`) instead. `common/`'s protocol and command
code doesn't know or care — it only ever sees `katcp_feed()` calls and
an output callback, on both targets.

## One-time toolchain setup

You'll need: `make`, an `arm-none-eabi-gcc` toolchain, and `git`. On
Debian/Ubuntu:

```bash
sudo apt install make gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git
```

You'll also need a way to flash/debug over SWD or JTAG — the E407 has
no built-in USB bootloader like the Pico, so an external probe is
required (an Olimex ARM-USB-OCD-H, a genuine ST-Link/V2, or a cheap
ST-Link clone all work). Install OpenOCD for this:

```bash
sudo apt install openocd
```

## ChibiOS (vendored as a submodule)

```bash
# from targets/stm32-e407/
git submodule add https://github.com/ChibiOS/ChibiOS.git third_party/ChibiOS
cd third_party/ChibiOS
git checkout stable_21.11.x   # the config files in ../../cfg were taken from this branch
git submodule update --init --recursive
```

ChibiOS vendors some external libraries (including lwIP) as compressed
`.7z` archives under `ext/` rather than plain source — decompress them
once after cloning:

```bash
# from targets/stm32-e407/third_party/ChibiOS/ext/
sudo apt install p7zip-full
for f in *.7z; do 7z x -y "$f"; done
```

## Build

```bash
# from targets/stm32-e407/
make -j4
```

This produces `build/katcp_stm32_e407.elf` (and `.bin`/`.hex` if your
ChibiOS version's rules generate them).

## Flash

Connect your SWD/JTAG probe to the board's JTAG header, then:

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/katcp_stm32_e407.elf verify reset exit"
```

Substitute `interface/stlink.cfg` for `interface/ftdi/olimex-arm-usb-ocd-h.cfg`
(or whatever matches your specific probe) if you're not using an
ST-Link-compatible one — run `openocd -f interface/... ` with no
target file and Ctrl-C to sanity-check the interface file loads before
committing to the full command.

## Before this does anything useful

Same two open items as `../rp2040-w5500-pico`, for the same reasons —
see that target's README and `../../IMPLEMENTATION.md` §6 for the full
rationale:

1. **Network config** — `mac_address` and the static IP/netmask/gateway
   in `src/main.c`'s `network_bringup()` are placeholders. The
   firmware tries DHCP for up to 30 seconds at boot (override with
   `-DDHCP_TIMEOUT_S=<n>` — add it to `UDEFS` in the Makefile) and
   falls back to the static values if no lease arrives in time. Change
   the static values to match your control network regardless, and
   give each unit a distinct MAC.

2. **Synth driver** — `../../common/src/synth_stub.c` always reports
   failure, exactly as on the RP2040 target. Once the specific IC and
   its wiring to this board are chosen, replace it with `synth_<part>.c`
   implementing the three functions in `../../common/include/synth.h`.
   Unlike the RP2040 target, SPI itself isn't even enabled yet in this
   target's `cfg/mcuconf.h`/`cfg/halconf.h` (`STM32_SPI_USE_SPI*` and
   `HAL_USE_SPI` are `FALSE`) — deliberately deferred, since which SPI
   peripheral and which pins depends on where the synth actually gets
   wired on this board, not something to guess at ahead of that
   decision.

## Testing the protocol without hardware

Identical to the RP2040 target — see `../../IMPLEMENTATION.md` §7 and
`../../test/platform_host.c`. The protocol/command/sensor code has no
ChibiOS or lwIP dependency and compiles as plain desktop C regardless
of which target you're about to flash it to.

Once running on real hardware, the same requests can be tried by hand
over the network:
```
$ nc 192.168.1.200 7147
?watchdog
!watchdog ok
```
