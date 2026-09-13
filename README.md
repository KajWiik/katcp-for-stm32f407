# katcp-for-w5500-evb-pico-rp2040-w5500

A KATCP server (radio-astronomy monitor-and-control protocol) for
controlling an SPI-driven synthesizer, designed to run on more than one
piece of hardware. The protocol, command handling, and sensor logic
are written once as plain, transport-agnostic C and shared across
every target; only the networking/SPI glue differs per board.

## Layout

```
common/            -- shared, target-agnostic code (protocol, commands, sensors, synth contract)
  include/
  src/
targets/           -- one directory per piece of hardware; each owns its own build
  rp2040-w5500-pico/   -- W5500-EVB-Pico, Pico SDK + WIZnet ioLibrary_Driver (built)
  stm32-e407/          -- Olimex STM32-E407, ChibiOS + lwIP, native MAC (planned)
test/              -- host-only helpers for building/running common/ as desktop C
IMPLEMENTATION.md  -- full design writeup and command reference (target-agnostic)
```

Start with `IMPLEMENTATION.md` for how the protocol and commands work.
Each target directory has its own README for board-specific setup,
build, and flashing instructions.

## Why split this way

`common/` has no dependency on any RTOS, vendor HAL, or board -- it's
plain ISO C, and can be compiled and tested as ordinary desktop code
(see `IMPLEMENTATION.md` section 7, and `test/platform_host.c`). Each
target under `targets/` supplies only what's genuinely
board-specific: the network transport, SPI glue, and a one-function
`platform_now_ms()` time source. Adding a new target means writing
that glue against the existing `common/include` contracts, not
touching or duplicating the protocol/command/sensor logic.
