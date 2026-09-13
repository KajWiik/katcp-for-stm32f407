# rp2040-w5500-pico

KATCP server for the W5500-EVB-Pico, controlling an SPI synthesizer.
Bare-metal Pico SDK, no Arduino. One of possibly several hardware
targets in this repo -- see the top-level README for how this fits
together with `../../common/` and other targets (e.g. `../stm32-e407`).

## Layout

```
src/
  main.c             -- Pico SDK + W5500 glue: SPI init, DHCP bring-up, TCP socket loop
  platform_pico.c    -- platform_now_ms() via Pico SDK's to_ms_since_boot()
CMakeLists.txt
```

The protocol, command table, sensor table, and synth driver contract
this target builds against live in `../../common/include` and
`../../common/src` -- they're shared with any other target in this
repo and don't know anything about the Pico SDK or W5500. See
`../../IMPLEMENTATION.md` for how all of that fits together and the
full command reference.

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

Copy the SDK's CMake bootstrap file into **this target directory**
(`targets/rp2040-w5500-pico/`) -- it's a small fixed file, not
something you edit:

```bash
cd targets/rp2040-w5500-pico
cp $PICO_SDK_PATH/external/pico_sdk_import.cmake .
```

## WIZnet ioLibrary_Driver (W5500 hardware driver)

Vendor it as a submodule rather than a plain clone, so the exact
commit is pinned. This lives under this target (not `common/`), since
it's RP2040/W5500-specific:

```bash
# from targets/rp2040-w5500-pico/
git submodule add https://github.com/Wiznet/ioLibrary_Driver.git third_party/ioLibrary_Driver
git submodule update --init --recursive
```

## Build

```bash
# from targets/rp2040-w5500-pico/
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

1. **Network config** -- `net_info` in `src/main.c` is a static
   placeholder (`192.168.1.200/24`, gateway `.1`). The firmware tries
   DHCP for up to 30 seconds at boot (override with
   `-DDHCP_TIMEOUT_S=<n>`) and falls back to this static config if no
   lease arrives in time -- see `../../IMPLEMENTATION.md` section 5.2
   for the full behavior. Change the static values to match your
   control network regardless, since they're what the device runs on
   whenever DHCP isn't available, and give each unit a distinct MAC
   (the locally-administered `02:00:00:00:00:01` placeholder must be
   unique per box on the same segment -- DHCP servers key leases on it
   too).

2. **Synth driver** -- `../../common/src/synth_stub.c` always reports
   failure. Once the specific IC is chosen, replace it with
   `synth_<part>.c` implementing the three functions in
   `../../common/include/synth.h` (`synth_init`, `synth_set_frequency`,
   `synth_is_locked`) -- the KATCP command layer doesn't need to
   change, and neither does any other target sharing that contract.

## Testing the protocol without hardware

See `../../IMPLEMENTATION.md` section 7 and `../../test/platform_host.c`
-- the protocol/command/sensor code has no Pico SDK or W5500 dependency
and compiles as plain desktop C.

Once running on real hardware, the same requests can be tried by hand
over the network:
```
$ nc 192.168.1.200 7147
?watchdog
!watchdog ok
```
