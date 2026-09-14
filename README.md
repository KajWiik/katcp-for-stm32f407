# katcp-for-w5500-evb-pico-rp2040-w5500

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
`targets/stm32-e407/README.md` has board-specific setup, build, and
flashing instructions.

## Why split this way

`common/` has no dependency on any RTOS, vendor HAL, or board -- it's
plain ISO C, and can be compiled and tested as ordinary desktop code
(see `IMPLEMENTATION.md` section 8, and `test/platform_host.c`). A
target under `targets/` supplies only what's genuinely board-specific:
the network transport, SPI glue, and a small `platform.h` contract
(monotonic time, and wall-clock time where available). Adding another
target later means writing that glue against the existing
`common/include` contracts, not touching or duplicating the
protocol/command/sensor logic.

Note: this repo previously also carried an RP2040 + W5500-EVB-Pico
target. It was removed once STM32F407 became the sole focus for new
hardware -- it's still recoverable from git history if ever needed,
but nothing in the current tree references it.
