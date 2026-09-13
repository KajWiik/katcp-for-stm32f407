# katcp-w5500-pico

Minimal KATCP server for the W5500-EVB-Pico, controlling an SPI
synthesizer. Bare-metal Pico SDK, no Arduino.

## Layout

```
include/
  katcp_proto.h      -- wire protocol: framing, escaping, dispatch (transport-agnostic)
  katcp_commands.h   -- the command table (?halt, ?watchdog, ?synth-frequency, ...)
  synth.h            -- fixed contract for the synth driver (chip-specific code not yet written)
src/
  katcp_proto.c      -- protocol implementation
  katcp_commands.c   -- command handlers
  synth_stub.c        -- placeholder synth driver; returns "not implemented"
  main.c             -- Pico SDK + W5500 glue: SPI init, TCP socket loop, feeds the parser
CMakeLists.txt
```

`katcp_proto.c` doesn't know about sockets or SPI at all -- it's plain C
you could unit-test on your desktop by feeding it byte arrays and
capturing what it hands to an `out()` callback. `main.c` is the only
file that knows this is running on a W5500-EVB-Pico.

## One-time toolchain setup

You'll need: `cmake`, `arm-none-eabi-gcc` toolchain, `git`, and the Pico
SDK itself. On Debian/Ubuntu:

```bash
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git
```

Clone the Pico SDK somewhere and point `PICO_SDK_PATH` at it (add this
to your shell rc file so it persists):

```bash
git clone -b master https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk
```

Copy the SDK's CMake bootstrap file into this project (it's a small
fixed file, not something you edit):

```bash
cp $PICO_SDK_PATH/external/pico_sdk_import.cmake .
```

## WIZnet ioLibrary_Driver (W5500 hardware driver)

Vendor it as a submodule rather than a plain clone, so the exact
commit is pinned:

```bash
git submodule add https://github.com/Wiznet/ioLibrary_Driver.git third_party/ioLibrary_Driver
git submodule update --init --recursive
```

## Build

```bash
mkdir build && cd build
cmake ..
make -j4
```

This produces `build/katcp_w5500_pico.uf2`.

## Flash

Hold BOOTSEL on the board while plugging in USB (or while pressing the
board's reset if BOOTSEL is wired separately), which mounts it as a USB
mass-storage device (`RPI-RP2`). Then:

```bash
cp build/katcp_w5500_pico.uf2 /media/$USER/RPI-RP2/
```

The board reboots and runs the firmware immediately -- no separate
"flash" step or debugger required for this basic workflow. (SWD +
`openocd`/`gdb` is available later if you want breakpoint debugging.)

## Before this does anything useful

Two things are still open, deliberately left as the next steps rather
than guessed at:

1. **Network config** -- `net_info` in `main.c` is a static placeholder
   (`192.168.1.200/24`, gateway `.1`). Change it to match your control
   network before flashing, and give each unit a distinct MAC (the
   locally-administered `02:00:00:00:00:01` placeholder must be unique
   per box on the same segment).

2. **Synth driver** -- `src/synth_stub.c` always reports failure.
   Once the specific IC is chosen, replace it with `synth_<part>.c`
   implementing the three functions in `include/synth.h`
   (`synth_init`, `synth_set_frequency`, `synth_is_locked`) --
   the KATCP command layer doesn't need to change.

## Testing the protocol without hardware

Since `katcp_proto.c` has no socket/SPI dependency, you can `nc` into
port 7147 once it's running on the network and try it by hand:

```
$ nc 192.168.1.200 7147
?watchdog
!watchdog ok
?help
#help halt Halt the device.
#help watchdog Check the server is alive.
...
!help ok 6
?synth-frequency 1000000000
!synth-frequency fail synth programming failed or out of range
```

That last `fail` is expected and correct until the real synth driver
replaces the stub.
