# katcp-for-stm32f407

A KATCP server (radio-astronomy monitor-and-control protocol) for
controlling an SPI-driven synthesizer. The protocol, command handling,
and sensor logic are written once as plain, target-agnostic C in
`common/`; hardware-specific glue lives under `targets/`, one
directory per board.

## Layout

```
common/            -- shared, target-agnostic code (protocol, commands, sensors, synth contract)
  include/
  src/
targets/           -- one directory per piece of hardware; each owns its own build
  stm32-e407/          -- Olimex STM32-E407, ChibiOS + lwIP, native Ethernet MAC
test/              -- host-only helpers for building/running common/ as desktop C
IMPLEMENTATION.md  -- full design writeup and command reference
```

Start with `IMPLEMENTATION.md` for how the protocol and commands work.
`targets/stm32-e407/README.md` has the full, current version of the
setup/build/flash steps below plus troubleshooting detail -- this
section is a condensed quick-start, not a separate source of truth.

## Building and flashing (STM32-E407)

**Toolchain** (Debian/Ubuntu):
```bash
sudo apt install make gcc-arm-none-eabi libnewlib-arm-none-eabi \
                  build-essential git openocd p7zip-full
```
You'll also need an SWD/JTAG probe (Olimex ARM-USB-OCD-H, a genuine
ST-Link/V2, or a cheap ST-Link clone) -- the E407 has no USB
bootloader.

**ChibiOS**, vendored as a submodule (pins the exact commit the
`cfg/*.h` files were transcribed from -- see the "submodule" question
below for why that matters):
```bash
cd targets/stm32-e407
git submodule add https://github.com/ChibiOS/ChibiOS.git third_party/ChibiOS
cd third_party/ChibiOS
git checkout stable_21.11.x
git submodule update --init --recursive
```
ChibiOS vendors some external libraries (including lwIP) as compressed
`.7z` archives rather than plain source -- decompress them once:
```bash
cd ext
for f in *.7z; do 7z x -y "$f"; done
```

**Build:**
```bash
cd targets/stm32-e407       # back to the target root
make -j4
```
Produces `build/katcp_stm32_e407.elf`.

**Flash**, with your SWD/JTAG probe connected to the board's JTAG
header:
```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/katcp_stm32_e407.elf verify reset exit"
```
Substitute `interface/stlink.cfg` for whichever interface config
matches your specific probe (e.g.
`interface/ftdi/olimex-arm-usb-ocd-h.cfg`) if it's not
ST-Link-compatible.

**Before this does anything useful on real hardware:** the network
config, NTP server address, and synth driver are all still
placeholders -- see `targets/stm32-e407/README.md` and
`IMPLEMENTATION.md` for what needs to change and why each one was left
that way rather than guessed at.

## Why split this way

`common/` has no dependency on any RTOS, vendor HAL, or board -- it's
plain ISO C, and can be compiled and tested as ordinary desktop code
(see `IMPLEMENTATION.md` section 8, and `test/platform_host.c`). A
target under `targets/` supplies only what's genuinely board-specific:
the network transport, SPI glue, and a small `platform.h` contract
(monotonic time, wall-clock time, and MCU temperature where
available). Adding another target later means writing that glue
against the existing `common/include` contracts, not touching or
duplicating the protocol/command/sensor logic.

Note: this repo previously also carried an RP2040 + W5500-EVB-Pico
target. It was removed once STM32F407 became the sole focus for new
hardware -- it's still recoverable from git history if ever needed,
but nothing in the current tree references it.
