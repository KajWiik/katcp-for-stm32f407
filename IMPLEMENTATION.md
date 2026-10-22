# KATCP Server — Implementation Notes & Command Reference

Sections 1–4 and 6–9 describe the shared design in `common/` --
protocol, command handling, sensors, and the synth driver contract --
target-agnostic and reusable by any future hardware target. Section 5
covers the current (and only) target, the Olimex STM32-E407, using
ChibiOS + lwIP against its native Ethernet MAC. For toolchain setup
and build steps, see `targets/stm32-e407/README.md`.

---

## 1. Design goals and non-goals

**Goals:**
- Speak enough of the KATCP wire protocol to be a compliant, if
  minimal, device: correct framing, escaping, message IDs, and the
  request/reply/inform semantics a KATCP client library expects.
- Keep the protocol logic transport-agnostic so it can be tested on a
  desktop with plain `gcc`, with no RTOS, vendor HAL, or hardware
  involved at all.
- Keep the synth driver behind a fixed, small interface so the choice
  of synthesizer IC doesn't ripple into the protocol or command layer.

**Explicit non-goals (for now):**
- The full KATCP core request set (`?sensor-sampling`, `?log-level`,
  client-side `?client-list`, etc.). Only the subset needed for this
  device is implemented — see §4.
- Multiple simultaneous client connections. One TCP socket, one client
  at a time (see §5.5).
- Retrying DHCP after the initial boot-time attempt. See §5.2 — this
  is a one-shot, timeout-and-fall-back design, not a persistent
  background retry.

---

## 2. File layout and responsibilities

```
common/                      -- shared, no RTOS/HAL dependency
  include/
    katcp_proto.h      Wire protocol types and API (transport-agnostic)
    katcp_commands.h   Command table declaration + halt flag
    synth.h            Fixed contract for the synth driver
    pac1954.h          Fixed contract for the PAC1954 power monitor + code-to-unit helpers (§6.4)
    web_ui.h           Browser front-end request handler, transport-agnostic (§11)
    sensor.h           Fixed contract for the sensor table + handlers
    platform.h         Monotonic + wall-clock time + MCU temperature abstraction (target vs. host builds)
  src/
    katcp_proto.c      Framing, escaping, tokenizing, dispatch
    katcp_commands.c   Command table and handler implementations
    synth_stub.c       Placeholder synth driver (always reports failure)
    sensor.c           Sensor table (synth.locked, board.uptime, mcu.temperature, pac1954.*) + handlers
    web_ui.c           HTML page + /api/status, /api/frequency routes (§11)
targets/
  stm32-e407/               -- see §5 and its own README.md
    src/
      main.c           ChibiOS/lwIP glue: network bring-up, KATCP-over-socket thread
      platform_chibios.c  platform.h implementation (RTC-backed wall clock)
      rtc_sync.c       SNTP-based RTC sync (§7)
      pac1954_i2c.c    PAC1954 driver over I2C1, background polling thread (§6.4)
      web_server.c/.h  HTTP-over-lwIP-socket thread on port 80 serving web_ui.c (§11)
      portab.c/.h      Minimal board hook (LED heartbeat)
    cfg/               chconf.h / halconf.h / mcuconf.h / lwipopts.h
    Makefile
test/
  platform_host.c      platform.h implementation for desktop tests (§8)
  pac1954_stub.c       pac1954.h stand-in with fixed raw codes for desktop tests (§8)
julia/
  KatcpClient/         General-purpose KATCP client package (§10.1)
  load_register_dump.jl  LMX2594 register-dump replay tool (§10.2)
  test_fixtures/       Real register-dump data used to test the above
```

The dependency direction is strict and one-way:

```
main.c  --->  katcp_commands.c  --->  katcp_proto.c
  |                  |         \
  v                  v          --->  sensor.c ---> platform.h
Ethernet MAC /    synth.h  --->  synth_stub.c (or a real driver later)
SPI (synth)
```

`katcp_proto.c` never includes anything from a vendor SDK/HAL,
`synth.h`, or `sensor.h`. It only knows about bytes in (`katcp_feed`)
and a caller-supplied output callback (`katcp_out_fn`). `sensor.c`
follows the same discipline via `platform.h`: it calls
`platform_now_ms()`/`platform_wallclock_unix_s()` rather than any
target-specific time API directly, which is what keeps it — like the
protocol and command layers — buildable and testable as plain desktop
C (§8).

---

## 3. Protocol implementation (`katcp_proto.c` / `.h`)

### 3.1 Wire format

Reference: NRF-KAT7-6.0-IFCE-002 (KATCP guidelines). Each message is
one newline-terminated ASCII line with one of three prefixes:

| Prefix | Meaning | Direction |
|---|---|---|
| `?name arg1 arg2 ...` | request | client → server |
| `!name status arg...` | reply (`status` = `ok`\|`fail`\|`invalid`) | server → client |
| `#name arg...` | inform (sync, inside a reply, or async) | server → client |

Both `\n` and `\r` are treated as line terminators by `katcp_feed()`
(a lone `\r` or `\n` both flush the current line; `\r\n` pairs just
flush twice on an empty second line, which is a no-op).

### 3.2 Escaping

Implemented exactly as specified, matching the SKA-SA reference
`parse.c` byte-for-byte on the escape table:

| Escape | Byte | Note |
|---|---|---|
| `\_` | space (0x20) | |
| `\n` | LF (0x0A) | |
| `\r` | CR (0x0D) | |
| `\t` | TAB (0x09) | |
| `\e` | ESC (0x1B) | |
| `\0` | NUL (0x00) | |
| `\@` | *(nothing)* | zero-width empty-argument marker |
| `\\` | `\` | literal backslash |

`unescape_arg()` handles incoming arguments in place; `emit_escaped_arg()`
handles outgoing ones, including emitting `\@` for a genuinely
zero-length argument so it round-trips correctly.

### 3.3 Line accumulation and tokenizing

`katcp_feed()` is designed to be called with whatever chunk size the
transport hands it (e.g. one `recv()`'s worth of bytes) — it doesn't
assume one call equals one line. Bytes accumulate in
`ctx->linebuf` until a line terminator is seen, at which point
`dispatch()` tokenizes and routes the complete line.

`KATCP_MAX_LINE` (256 bytes) bounds a single request. A line that
exceeds this is dropped in full (not silently truncated), and parsing
resynchronizes on the next newline — this avoids ever executing a
command against a truncated/corrupted argument.

`tokenize()` splits on spaces, respecting backslash-escapes so an
escaped space (`\_`) doesn't end an argument early. It null-terminates
each argument in place inside `linebuf` after unescaping.

> **Implementation note (fixed during development):** the separator
> byte between two arguments must be explicitly consumed (`p++`,
> `remaining--`) *before* the current argument's null terminator is
> written. The null write lands at the same offset the separator
> occupied whenever an argument contains no escapes, so writing it
> before advancing `p` corrupts the separator the *next* argument's
> whitespace-skip depends on — silently turning `?cmd arg1 arg2` into
> `arg1` plus an empty second argument. This was caught by the host-side
> test harness (§8), not by inspection, which is why that harness is
> worth keeping and extending as commands are added.

`argv[0]` may carry a KATCP message ID suffix, e.g. `halt[7]` — this is
split off into `mid` before dispatch, independent of tokenizing.

### 3.4 Message IDs (`[mid]`)

If a request arrives as `?halt[7]`, all replies and synchronous
informs for that request are automatically tagged `[7]`
(`!halt[7] ok`). This is handled transparently by `katcp_reply()` /
`katcp_inform()` via `ctx->cur_mid` — command handlers never see or
handle the mid themselves. `katcp_inform_async()` deliberately omits
the mid, since async informs aren't a reply to any specific request.

### 3.5 Dispatch

`dispatch()` does a linear scan of the command table
(`katcp_command_table`, defined in `katcp_commands.c`) comparing
`argv[0]` against each `.name`. On no match, it replies
`!<name> invalid unknown request` — KATCP requires an explicit invalid
reply here, not silence. On a match, the handler is called directly
(single-threaded, no queuing); the handler is expected to call
`katcp_reply()` exactly once before returning.

### 3.6 Public API surface (`katcp_proto.h`)

| Function | Called by | Purpose |
|---|---|---|
| `katcp_init()` | `main.c`, once at startup | Wire up the command table and output callback |
| `katcp_feed()` | `main.c`, per `recv()` | Feed raw bytes in |
| `katcp_reply()` | command handlers | Send the one required reply to the current request |
| `katcp_inform()` | command handlers | Send a synchronous inform (tagged with the current mid) |
| `katcp_inform_async()` | anywhere, any time | Send an unsolicited inform (no mid) |

---

## 4. Available commands

All nine requests currently implemented, in `katcp_commands.c`. Every
handler replies exactly once, as KATCP requires.

### `?halt`
Halts the device.
- **Args:** none
- **Reply:** `!halt ok`
- **Side effect:** sets `katcp_halt_requested = 1`. `main.c` checks this
  flag on the active connection; the current implementation just
  disconnects the client (§5.5) — it does **not** reset the board or
  park the synth in a safe state. That's a deliberate placeholder:
  "halt" needs a project-specific definition (should the synth be
  zeroed first? should the board actually reboot?) rather than a
  guessed-at one.

### `?watchdog`
Liveness check.
- **Args:** none
- **Reply:** `!watchdog ok`

### `?version-list`
Reports device identity.
- **Args:** none
- **Informs:** `#version-list <name> <version>` — one inform, using the
  `DEVICE_NAME` / `DEVICE_VERSION` constants at the top of
  `katcp_commands.c` (currently `"synth-katcp-bridge"` / `"0.1"`)
- **Reply:** `!version-list ok 1`

### `?help [request-name]`
Lists available requests, or describes one.
- **Args:** optional request name
- **No argument:** emits one `#help <name> <description>` inform per
  entry in the command table, then `!help ok <count>`
- **With argument:** emits a single matching `#help` inform and
  `!help ok 1`, or `!help fail unknown request` if no such command
  exists
- **Example:**
  ```
  ?help
  #help halt Halt the device.
  #help watchdog Check the server is alive.
  #help version-list List component name and version.
  #help help Describe available requests.
  #help synth-frequency Set synth output frequency in Hz.
  #help synth-locked Report synth PLL lock status (0/1).
  #help synth-write-register Write one raw 24-bit synth register word.
  #help sensor-list List available sensors, or describe one.
  #help sensor-value Report current sensor value(s).
  !help ok 9
  ```

### `?synth-frequency <hz>`
**Custom, non-core request.** Sets the synthesizer output frequency.
- **Args:** exactly one, an unsigned integer number of Hz
- **Reply (bad argument count):** `!synth-frequency invalid usage: ?synth-frequency <hz>`
- **Reply (non-numeric argument):** `!synth-frequency invalid frequency must be an integer in Hz`
- **Reply (numeric, but driver rejects it):** `!synth-frequency fail synth programming failed or out of range`
- **Reply (success):** `!synth-frequency ok`
- **Current status:** always replies `fail`, because `synth_stub.c` is a
  placeholder (see §6). The parsing and reply logic above is fully
  implemented and tested; only `synth_set_frequency()` itself is not.

### `?synth-locked`
**Custom, non-core request.** Reports PLL lock status.
- **Args:** none
- **Reply:** `!synth-locked ok 0` or `!synth-locked ok 1`
- **Current status:** always reports `0` (unlocked), since
  `synth_is_locked()` is stubbed.

### `?synth-write-register <word24>`
**Custom, non-core request.** Writes one raw 24-bit register word
verbatim — the one-request-per-register path for replaying a
precomputed register dump (e.g. from PLLatinumSim), as opposed to
`?synth-frequency`'s "compute everything from a target frequency"
approach. See §6.3 for why these are genuinely different problems, not
two ways of doing the same thing, and §10 for the Julia tooling built
to drive this one.
- **Args:** exactly one, a 24-bit integer (hex `0x...` or decimal —
  `strtoul(..., 0)` auto-detects)
- **Reply (bad argument count):** `!synth-write-register invalid usage: ?synth-write-register <24bit-value>`
- **Reply (out of range):** `!synth-write-register invalid value must be a 24-bit integer (0x000000-0xFFFFFF)`
- **Reply (in range, but driver rejects it):** `!synth-write-register fail synth register write failed`
- **Reply (success):** `!synth-write-register ok`
- **Current status:** always replies `fail`, same placeholder reason
  as `?synth-frequency` — `synth_write_raw_register()` in
  `synth_stub.c` isn't implemented yet.
- **On the wire format:** `word24` is bits `[23:16]` = register
  address, bits `[15:0]` = data — exactly the raw SPI word an
  LMX2594-style part expects, and exactly what a register-dump export
  file already contains per line (e.g. `R112\t0x700000`). No
  interpretation happens beyond range-checking; the client is
  responsible for getting the value right.
- **Pipelining:** each write is a separate request, but nothing stops
  a client from sending many in one `send()`/one `recv()`-sized chunk,
  each tagged with a distinct `[mid]` — the tokenizer already handles
  multiple complete requests arriving in a single `katcp_feed()` call
  in strict order (§3.3), so this needs no server-side support beyond
  what already exists. This matters for register dumps specifically:
  LMX2594-style parts require register 0 (which triggers VCO
  calibration) to be written *last*, and processing order is
  preserved regardless of which order replies come back in — see §10.

### Unimplemented request
Anything not in the table above:
```
?bogus
!bogus invalid unknown request
```

### `?sensor-list [sensor-name]`
Lists available sensors, or describes one. Structurally identical to
`?help`, walking a static table (`sensor_table` in `sensor.c`) instead
of the command table.
- **No argument:** one `#sensor-list` inform per sensor, then
  `!sensor-list ok <count>`
- **Inform format:** `#sensor-list <name> <description> <units> <type> [min max]`
  — `min`/`max` are appended for `integer`- and `float`-typed sensors
  (for `float`, formatted with `%.*f` from a genuine `double` — see
  §6.2)
- **With argument:** single matching inform + `!sensor-list ok 1`, or
  `!sensor-list fail unknown sensor`
- **Currently defined sensors:**

  | Name | Type | Units | Range | Backed by |
  |---|---|---|---|---|
  | `synth.locked` | boolean | *(none)* | 0–1 | `synth_is_locked()` |
  | `board.uptime` | integer | seconds | 0–INT64_MAX | `platform_now_ms()/1000` |
  | `mcu.temperature` | float | degrees_C | −40–125 | `platform_mcu_temperature_c()` (§6.2) |
  | `pac1954.ch<N>.voltage` | float | volts | 0–32 | PAC1954 VBUS, N = 1–4 (§6.4) |
  | `pac1954.ch<N>.current` | float | amperes | 0–0.1/R<sub>sense</sub> | PAC1954 VSENSE / shunt (§6.4) |
  | `pac1954.ch<N>.power` | float | watts | 0–32·0.1/R<sub>sense</sub> | voltage × current, same refresh (§6.4) |

- **Example:**
  ```
  ?sensor-list
  #sensor-list synth.locked Synthesizer PLL lock status \@ boolean
  #sensor-list board.uptime Seconds since boot seconds integer 0 9223372036854775807
  #sensor-list mcu.temperature STM32F407 die temperature (uncalibrated ADC estimate) degrees_C float -40.000 125.000
  !sensor-list ok 3
  ```
  (The `\@` is the empty-argument marker from §3.2 — `synth.locked` has
  no units, so an empty argument is correctly escaped rather than
  silently dropped.)

### `?sensor-value [sensor-name]`
Reports current sensor value(s).
- **No argument:** one `#sensor-value` inform per sensor, then
  `!sensor-value ok <count>`
- **Inform format:** `#sensor-value <timestamp> <count> <name> <status> <value>`
  — `count` is always `1` here (this implementation reports one
  sensor per inform rather than batching several under one
  timestamp, which is a KATCP-legal simplification)
- **Timestamp:** UTC Unix epoch **seconds** (matching KATCP v5, which
  moved timestamps from milliseconds to seconds), from
  `platform_wallclock_unix_s()` — see §7 for how that's kept accurate.
  Reports `0` if the device has no synced wall clock yet (an honest
  "unsynced" signal, not a fake-looking placeholder number) — see §7
  for exactly when that applies.
- **Status word:** `nominal`, `warn`, `error`, or `unknown` — the
  `synth.*`/`board.*`/`mcu.*` sensors always report `nominal` (the read
  itself can't fail; see the comment in `sensor.c` on
  `read_synth_locked` for why an *unlocked* synth is reported as a
  nominal `0` value, not a sensor error). The `pac1954.*` sensors
  report `error` (value `0`) when the chip has no fresh reading — not
  on the bus, I2C errors, or the last good poll older than 1 s (§6.4)
- **With argument:** single sensor's value + `!sensor-value ok 1`, or
  `!sensor-value fail unknown sensor`
- **Example** (from a live run, with the RTC already synced):
  ```
  ?sensor-value
  #sensor-value 1789413904 1 synth.locked nominal 0
  #sensor-value 1789413904 1 board.uptime nominal 35
  #sensor-value 1789413904 1 mcu.temperature nominal 23.456
  !sensor-value ok 3
  ```

> **Not implemented:** `?sensor-sampling` (periodic/async sensor
> push). Both commands above are purely request/reply — nothing is
> pushed to the client without being asked. See §9.

---

## 5. STM32-E407 transport layer (`targets/stm32-e407/src/main.c`)

ChibiOS + lwIP against the STM32F407's native Ethernet MAC — no W5500
or other SPI Ethernet chip is involved. Board pin/PHY configuration
comes entirely from ChibiOS's own `OLIMEX_STM32_E407_REV_D` board
files (Rev E+ hardware, LAN8710A PHY — see the Makefile comment on why
this specific board file, not the plain `OLIMEX_STM32_E407`, matters:
the latter targets the older KS8721 PHY and will fail PHY detection on
this hardware).

### 5.1 Network configuration

Two-stage: DHCP first, static fallback — `network_bringup()` in
`main.c`.

1. `lwipInit()` is called with `addrMode = NET_ADDRESS_DHCP`, which
   starts DHCP as a side effect and returns immediately; lease
   negotiation happens asynchronously in lwIP's own tcpip thread.
2. A loop then polls `dhcp_supplied_address(netif_default)` for up to
   `DHCP_TIMEOUT_S` (default **30**, overridable at build time) seconds.
   - **Lease obtained in time:** the loop simply exits; the netif
     already has its DHCP-assigned address.
   - **Timeout:** `lwipReconfigure()` is called with
     `addrMode = NET_ADDRESS_STATIC` and the same static
     IP/netmask/gateway values `network_bringup()` started with —
     this internally stops the DHCP client and applies the static
     config (see lwIP's `lwipthread.c`, `do_reconfigure()`).

**This is a boot-time decision only.** If no DHCP server is present at
startup, the device runs on the static config for its entire uptime,
even if a server appears later — `network_bringup()` is never called
again automatically. Re-attempting DHCP later (e.g. from a
`?net-renew`-style command) is a natural extension but not implemented.

The static fallback config (`mac_address`, and the IP/netmask/gateway
literals in `network_bringup()`) must be changed per deployment, and
the MAC must be unique per unit — this matters for DHCP too, since
servers key leases on it.

### 5.2 Socket handling

One TCP socket, listening on port **7147** (the conventional
KATCP/`tcpborphserver` port), served from a dedicated ChibiOS thread
(`katcp_thread`) using lwIP's blocking BSD socket API
(`socket()`/`bind()`/`listen()`/`accept()`/`recv()`/`send()`). This is
a natural fit for lwIP + an RTOS — blocking socket calls yield the
thread rather than needing a hand-rolled polling state machine. The
accept loop only ever services one client connection at a time (see
§5.4); a fresh `katcp_ctx_t` is created per accepted connection.

### 5.3 Output path

`katcp_out_to_socket()` is the `katcp_out_fn` callback: every reply or
inform line the protocol layer produces is sent immediately via a
single `send()` call. There is no output buffering/batching — a
`?help` with many informs currently does one `send()` per line rather
than coalescing them into fewer TCP segments. Fine at this command
rate; worth revisiting only if `?help`-sized bursts ever become a
bottleneck.

### 5.4 One client at a time

The accept loop blocks on `accept()`, services one connection to
completion (until it closes or `katcp_halt_requested` fires), then
loops back to `accept()` again. A second client attempting to connect
while one is already being served will simply not be accepted until
the first disconnects. KATCP itself permits multiple concurrent
clients; this implementation doesn't yet, which is fine for a
single-controller setup but worth knowing if two systems ever expect
to talk to this box at once.

### 5.5 Heartbeat

`main()` becomes an idle-priority loop once setup (`network_bringup()`,
`rtc_sync_start()`, `synth_init()`, spawning `katcp_thread`) is done —
it just toggles the board LED every 500 ms as a simple "still alive"
indicator, separate from and not required by the KATCP protocol itself.

### 5.6 Build status: compiles and links cleanly against the real toolchain

Every previous claim in this document about "compiles" for this target
was necessarily aspirational — no `arm-none-eabi-gcc` toolchain was
available while most of this was written. Once one became available,
`make -j4` was actually run against this target, from a clean tree
(submodule at the exact pinned commit, `ext/lwip-2.1.2.7z` extracted),
and — after one real fix — it succeeds completely:
`build/katcp_stm32_e407.elf`/`.hex`/`.bin` all produced.

**The one real problem found, unrelated to anything float/FPU:** the
final link failed with undefined references to `_write`, `_sbrk`,
`_read`, `_close`, `_lseek`, `_exit`, `_kill`, `_getpid`, `_fstat`,
`_isatty` — newlib's own reentrant wrappers (`_write_r` etc., in
`libg.a`) are thin shims that call these plain, non-reentrant versions,
which nothing in this project had ever supplied. This is a standard
bare-metal newlib bring-up gap, not something introduced by any recent
change — it was simply never triggered before because this was the
project's first real link attempt. Fixed by adding
`$(CHIBIOS)/os/various/newlib_bindings/syscalls.c` to `CSRC`
(Makefile) — ChibiOS's own file for exactly this purpose, supplying
the reentrant versions directly (`_sbrk_r` genuinely backed by
ChibiOS's own heap via `chCoreAllocFromBase`, not a dummy stub) rather
than the plain ones newlib expects, which preempts the archive members
that caused the undefined references in the first place.

**Resulting image, for reference:**

| | Size | Note |
|---|---|---|
| `.text` (flash) | 99,328 B | ~9.5% of the F407's 1 MB flash |
| `.data` + `.bss` (RAM) | 131,072 B | ~66% of the F407's 192 KB SRAM |

The RAM figure looks large at a glance but breaks down into
legitimate, expected consumers, not a leak or misconfiguration
(checked via `arm-none-eabi-nm --size-sort`, not assumed): lwIP's
`PBUF_POOL` (~9.7 KB), the ChibiOS core heap backing `_sbrk_r` above
(~8.2 KB, matching `MEM_SIZE=8192` in `cfg/lwipopts.h` exactly), the
Ethernet MAC's DMA rx/tx buffers (~8.4 KB combined,
`STM32_MAC_RECEIVE_BUFFERS`/`TRANSMIT_BUFFERS` in `cfg/mcuconf.h`),
and per-thread stacks including `wa_katcp_thread` (2.4 KB). ~66 KB of
SRAM remains free — comfortable for now, worth watching if
significantly more gets added later (a real synth driver, more
sensors, a second client, etc.) but not a concern today.

**Still not verified:** actually flashing and running this on real
hardware. A clean build is a real, meaningful checkpoint — it confirms
every source file, every config flag, and the whole dependency chain
are internally consistent — but it says nothing about runtime
behavior (PHY bring-up, RTC/SNTP actually working, ADC readings being
sane, etc.). That's the next real test, not this one.

**FPU on vs. off, measured, not assumed** (prompted by a direct
question about whether the RAM figure above already includes
`USE_FPU=hard` — confirmed yes, via `arm-none-eabi-readelf -A`
showing `Tag_ABI_VFP_args: VFP registers`). A real side-by-side build
with `USE_FPU=no` (`make USE_FPU=no BUILDDIR=./build_nofpu`):

| | `USE_FPU=hard` | `USE_FPU=no` |
|---|---|---|
| `.text` (flash) | 99,328 B | 99,760 B (432 B *more*) |
| `.data`+`.bss` (RAM) | 131,072 B | 131,072 B (identical) |

See §6.1 for what this comparison does and doesn't tell you — static
image size is unaffected either way, but that's not the full FPU cost
story.

---

## 6. Synth driver contract (`common/include/synth.h` / `common/src/synth_stub.c`)

### 6.1 Floats are now used deliberately -- this reverses an earlier decision

**This project originally avoided `float`/`double` everywhere** in
favor of scaled integers (e.g. millidegrees) with hand-rolled
fixed-point text formatting. That decision has been reversed, and
it's worth recording both the original reasoning and why it changed,
rather than quietly rewriting history.

**The original reasoning (no longer the operative one):** the
STM32F407 has a hardware FPU (Cortex-M4F), but early on this project
didn't use it (`USE_FPU = no`), out of general caution that `printf`
`%f` support isn't reliably linked in by the newlib-nano C library
configuration common in embedded toolchains.

**Why it changed:** two things. First, the synth loop-setup math this
project actually needs to implement -- charge-pump current and
VCO-tuning-curve selection for `?synth-frequency` (§6.3) -- genuinely
benefits from real floating point rather than being forced through
fixed-point arithmetic. Second, and more directly: the newlib-nano
concern was **checked against this project's actual build, not just
assumed** -- this Makefile never sets `--specs=nano.specs` anywhere
(confirmed by grepping ChibiOS's own build rules), so full newlib is
what's actually linked, and full newlib's `%f` support works with no
special linker flags needed. The caution that justified avoiding
floats didn't actually apply to this specific toolchain configuration.

**What changed as a result:**
- `targets/stm32-e407/Makefile`: `USE_FPU = hard` (was `no`) -- real
  hardware FPU instructions, hard-float ABI. The accepted cost is a
  larger FPU register bank (`S0`-`S31` + `FPSCR`) saved/restored on
  every context switch for any thread touching floating point, judged
  reasonable for this application. **This is a dynamic, per-context-
  switch stack cost, not a static image-size cost** -- worth being
  precise about the distinction, since it's easy to conflate the two.
  A real side-by-side build (§5.6) confirmed `.bss`/`.data` are
  byte-for-byte identical between `USE_FPU=hard` and `USE_FPU=no`, and
  flash is actually *smaller* with the FPU on (native instructions are
  more compact than software float-emulation routines) -- neither of
  which the "register bank cost" framing above should be read as
  contradicting. What that comparison can't see is ARM's lazy FPU
  stacking (`S16`-`S31` pushed onto a thread's own stack on certain
  exception entries), which doesn't show up as a static `.bss` size
  difference because `THD_WORKING_AREA()` sizes are fixed arrays
  sized by whatever literal was passed in, not auto-grown for FPU
  margin. None of this project's current code exercises the FPU
  inside an interrupt-adjacent context enough to matter, so it's not
  a live problem today -- but it's a real question to actually check
  (ChibiOS's `CH_DBG_FILL_THREADS` stack high-water-mark
  instrumentation, or computing the worst-case lazy-stack frame
  directly) once the synth loop-setup math (§6.3) is written and
  genuinely uses the FPU from within `katcp_thread`, rather than
  assumed fine because a comparison that structurally couldn't detect
  it came back clean.
- `sensor.h`'s `SENSOR_FLOAT` sensors now hold a genuine `double` (via
  a `sensor_value_t` union alongside the `int64_t` used for
  `SENSOR_INTEGER`/`SENSOR_BOOLEAN`), formatted with plain `%.*f` --
  see §6.2. The old `scale`-field/`format_fixed()` scaled-integer
  machinery is gone.
- `platform_mcu_millidegrees_c()` became `platform_mcu_temperature_c()`,
  returning degrees Celsius directly as a `double` -- see §6.2.
- `?synth-frequency`'s eventual real implementation (§6.3, still
  stubbed) is now free to use real floating-point arithmetic for the
  charge-pump/VCO-curve math rather than being constrained to
  fixed-point.

**What's unchanged:** `?synth-frequency` and `?synth-write-register`
still use integer Hz / a raw 24-bit integer word on the wire --
frequency and raw register values are naturally integer quantities
regardless of the FPU question, this reversal is specifically about
where genuine floating-point *math* or *display* is needed, not a
blanket "use floats everywhere now."

Three functions, chip-agnostic by design:

```c
void synth_init(void);                          // one-time SPI/GPIO setup
bool synth_set_frequency(uint64_t freq_hz);      // program the output
bool synth_is_locked(void);                      // lock-detect status
```

`synth_stub.c` implements all three as no-ops / `false`, so the KATCP
server is fully network-testable (including realistic `fail` replies
from `?synth-frequency`) before any chip-specific SPI code exists. To
bring up a real synthesizer, replace `synth_stub.c` with e.g.
`synth_lmx2594.c` implementing the same three functions against that
chip's register map — nothing in `katcp_commands.c` or `katcp_proto.c`
needs to change. SPI itself isn't even enabled yet in
`targets/stm32-e407/cfg/mcuconf.h`/`halconf.h` — deliberately deferred
until the chip and its wiring to this board are chosen.

### 6.2 `mcu.temperature` — the first `SENSOR_FLOAT` sensor

KATCP's spec defines a real `float` sensor type alongside
`integer`/`boolean`/etc. Once a genuinely continuous quantity showed
up (die temperature), it was worth actually exposing `SENSOR_FLOAT` as
a type rather than quietly reporting a temperature as `SENSOR_INTEGER`
with `units="millidegrees_C"` and letting the client guess.

**`SENSOR_FLOAT` now holds a genuine C `double`** (§6.1) — `sensor_def_t`
has a `sensor_value_t min, max` (a union of `int64_t i` / `double f`,
selected by the sensor's `type`) and an `int decimals` field used
directly as `%.*f`'s precision argument, e.g. `decimals=3` → `"23.456"`.
`sensor.c`'s `emit_sensor_list_entry()`/`emit_sensor_value_entry()`
format `SENSOR_FLOAT` values and `min`/`max` with plain
`snprintf(..., "%.*f", s->decimals, value)` — no hand-rolled
fixed-point arithmetic. Verified after the change (not just assumed to
still work): compiled and run on the host with a deliberately negative
fake temperature (`-12.3456`), confirming standard `%.*f` handles the
sign and rounds correctly (`"-12.346"`) — the exact case the old
hand-rolled formatter needed a negate-then-add-1 trick to get right,
now handled by the standard library instead of custom logic.

**The sensor itself:** `mcu.temperature`, backed by
`platform_mcu_temperature_c()` (`platform.h` contract function,
returning degrees Celsius as a `double`; implemented for real in
`targets/stm32-e407/src/platform_chibios.c`, stubbed to a fixed fake
value in the desktop test build, same pattern as
`platform_wallclock_unix_s()`). Reads the STM32F407's internal
temperature sensor via ADC1 channel 16 (`ADC_CHANNEL_SENSOR`) — no
GPIO setup needed, it's a purely internal channel, unlike a regular
ANx pin. `HAL_USE_ADC`/`STM32_ADC_USE_ADC1` are `TRUE` in
`cfg/halconf.h`/`cfg/mcuconf.h`. The ADC group config (sample time
`ADC_SAMPLE_144` for the sensor channel specifically) is modeled
directly on ChibiOS's own `testhal/STM32/STM32F4xx/ADC/main.c`
example, which demonstrates this exact channel — not derived from
scratch. The V25/Avg_Slope-to-temperature conversion itself is now
plain double arithmetic (`(v_sense_v - 0.76) / 0.0025 + 25.0`) —
noticeably more readable than the old scaled-integer version, a small
side benefit of §6.1's reversal beyond the synth math it was mainly
made for.

**Accuracy: deliberately uncalibrated, and worth being honest about
how rough that is.** The conversion uses the STM32F407 datasheet's
*typical* `V25`/`Avg_Slope` values (0.76 V, 2.5 mV/°C), not a
per-chip factory calibration. The F407 does have a single-point
calibration value (`TS_CAL1`) in system memory that could correct the
offset — but no address/format for it was independently verified
against the actual reference manual for this specific part before
writing this code, so rather than risk reading the wrong flash address
or misapplying the format, this implementation just doesn't use it.
The result should be treated as a rough die-temperature indicator (and
notably, die temperature, not ambient — self-heating and PCB thermal
coupling both matter) rather than a calibrated thermometer. Adding
`TS_CAL1`-based offset correction is a reasonable improvement *once*
that's confirmed against ST's actual documentation for this part —
deliberately not attempted speculatively here.

### 6.3 Register dump replay vs. computed frequency — two different problems

`?synth-write-register` (§4) and `?synth-frequency` (§4) look like two
ways to do the same thing but aren't, and it's worth being explicit
about why rather than letting that stay implicit.

**`?synth-write-register` is foolproof but not useful for a frequency
sweep.** It replays an already-computed register set byte-for-byte —
no interpretation, no math, just range-checked SPI writes. That's
exactly right for loading a known-good configuration, but useless for
programming an arbitrary frequency on demand, because getting an
LMX2594-class part to a specific frequency correctly requires more
than picking register values for the right N-divider: **the charge
pump current has to be set correctly for whichever of the part's
multiple VCO tuning-curve regions the target frequency falls into** —
get that wrong and the loop either doesn't lock cleanly or locks with
degraded phase noise. A register dump captures one specific frequency
that's already had this worked out (by PLLatinumSim or similar); it
says nothing about how to compute it for an arbitrary next frequency.

**`?synth-frequency` is the right shape for a sweep, but isn't
implemented, and doing so is a real, separate piece of work** — port
the frequency-to-register-set math (N-divider calculation, VCO-curve
selection, charge-pump-current lookup) from wherever that logic
currently lives (PLLatinumSim), not just wire up a stub. This is
precisely the math that motivated enabling the FPU and switching to
real floating point (§6.1) — whether the actual port ends up using
it throughout, or only in places, is still a question for whoever
does that work, not answered speculatively here; the point of §6.1's
reversal was removing the *constraint* against using floats, not
deciding in advance exactly where they'll be used.

**Practical consequence:** get the one-request-per-register path
working first (§4, §10) since it's foolproof and immediately useful
for loading known-good configurations — which is exactly what's been
built so far. `?synth-frequency` staying stubbed is a real, open gap
(§9), not an oversight.

### 6.4 PAC1954 power monitor (`pac1954.h` / `targets/stm32-e407/src/pac1954_i2c.c`)

Microchip PAC1954: four-channel I2C bus-voltage/current/power monitor.
Exposed as twelve `SENSOR_FLOAT` sensors, `pac1954.ch<N>.voltage`,
`.current` and `.power` for N = 1–4 (§4).

**Split, same discipline as `synth.h`:** `common/include/pac1954.h` is
the contract; the target driver only returns raw 16-bit register codes
(`pac1954_read_raw()`), and all code-to-unit conversion
(`pac1954_vbus_volts()`, `pac1954_vsense_volts()`, division by the
shunt resistance, V×I for power) lives in `common/` — so it compiles
and is exercised in the desktop build (§8) via `test/pac1954_stub.c`.

**Driver (`pac1954_i2c.c`):** I2C1 at 400 kHz on PB8 (SCL) / PB9
(SDA) — the E407's UEXT I2C, already muxed as AF4 open-drain by the
ChibiOS `OLIMEX_STM32_E407_REV_D` board file. `HAL_USE_I2C` and
`STM32_I2C_USE_I2C1` are now `TRUE` (I2C1 DMA streams 1/0 and 1/6 don't
collide with SPI1's or ADC1's on DMA2). A `NORMALPRIO-1` thread polls
every 100 ms: `REFRESH_V` (0x1F — latches new data *without* resetting
the accumulators, unlike plain `REFRESH`), waits 2 ms (datasheet
minimum is 1 ms), then reads `VBUSn_AVG` (0x0F–0x12) and
`VSENSEn_AVG` (0x13–0x16), the chip's rolling 8-sample averages. Each
register is its own 2-byte transaction rather than one block read,
because block reads on this family skip disabled channels' registers.
A full successful poll is published as one mutex-protected snapshot,
so `?sensor-value`'s voltage, current and power for a channel always
come from the same refresh, and the sensor read never touches the bus
(`sensor.h`'s "must not block"). A timed-out transfer stops and
restarts the I2C driver, as ChibiOS's `hal_i2c` requires.

**Assumptions to check against real hardware** (all marked
`CONFIGURE` in the source):
- **I2C address** `0x10` — placeholder; the real value is set by the
  ADDRSEL resistor strap.
- **Shunt resistors** `PAC1954_RSENSE_CH<N>_OHMS`, default 10 mΩ
  (10 A full scale). Override per build, e.g.
  `make UDEFS=-DPAC1954_RSENSE_CH1_OHMS=0.05`. A wrong value scales
  current and power silently; voltage is unaffected.
- **Power-on default configuration** — 1024 SPS, all channels on,
  **unipolar** ranges (VBUS 0–32 V, VSENSE 0–100 mV, `NEG_PWR_FSR` =
  0). Nothing writes `CTRL`/`NEG_PWR_FSR`; enabling bipolar ranges
  would require changing the conversion helpers too.
- No Product/Manufacturer ID check is done; "present" just means the
  chip ACKs and returns data.

Verified: host build (§8) shows the expected conversions from the
stub's fixed codes (e.g. VBUS 10240 → 5.0000 V, VSENSE 32768 at 10 mΩ →
5.0000 A, 25.0000 W) and the `error` path; the STM32 target builds
cleanly with no warnings. **Not yet verified on real hardware.**

---

## 7. RTC time sync (`targets/stm32-e407/src/rtc_sync.c`)

**Why this exists:** KATCP v5 specifies sensor timestamps as UTC
wall-clock time, not device uptime. `?sensor-value`'s timestamp field
(§4) is sourced from `platform_wallclock_unix_s()`, which is backed by
this module.

**Design:** boot-time sync plus resync every 6 hours, entirely via
lwIP's own SNTP client module — no polling loop or timer of our own.
Setting `SNTP_UPDATE_DELAY` (`cfg/lwipopts.h`) to 6h is the *entire*
resync mechanism; lwIP handles the scheduling, retry, and request/reply
internally. `rtc_sync_start()` (called once from `main()`, after
`network_bringup()`) just points it at a server and calls `sntp_init()`.

**No battery backup needed.** Since time is always re-fetched from the
network rather than relied on to survive a power cycle, there's no
coin-cell/VBAT requirement — this was the deciding factor in choosing
this design over trying to preserve RTC state across power loss.

**Clock source:** `cfg/mcuconf.h` selects `STM32_RTCSEL_LSE`, i.e. the
RTC is driven by a 32.768 kHz crystal on the dedicated
`OSC32_IN`/`OSC32_OUT` pins (PC14/PC15), not the internal RC
oscillator — LSI's several-percent drift isn't good enough for
anything worth calling "accurate," LSE gets you to tens of ppm.
`OLIMEX_STM32_E407_REV_D`'s board file declares `STM32_LSECLK 32768`
and wires those pins for it, implying Olimex populated the crystal —
**worth confirming against your specific board/revision rather than
assuming**, since this wasn't independently verified against Olimex's
schematic.

**Local server, not a public pool.** `sntp_setserver()` is called with
a single static IP (`ntp_server_ip` in `main.c`, currently a
placeholder — set to this site's maser/GNSS-disciplined time server
before deploying). `SNTP_SERVER_DNS` is left off (0), so no DNS
resolution is needed. This choice is unrelated to the project's
radio-quiet-site networking decisions elsewhere (wired Ethernet, no
on-chip radios) — it's simply that a locally maintained reference is
better than anything a public pool could offer, nothing about RF
quietness specifically motivates it.

**Unix epoch ↔ UTC calendar conversion is hand-rolled, deliberately —
not libc's `gmtime()`/`mktime()`.** This avoids depending on newlib's
timezone handling being correctly configured in this embedded setup.
It's the public-domain Howard Hinnant `civil_from_days`/`days_from_civil`
algorithm; before being used here it was compiled and run standalone on
a desktop and checked byte-for-byte against glibc's `gmtime()` across
the epoch boundary, a leap day, the year-2038 signed-32-bit overflow
point, and the `uint32_t` maximum (~2106) — all matched exactly,
including round-tripping back to the original epoch value.

**lwIP's `SNTP_SET_SYSTEM_TIME(sec)` macro already hands you a Unix
epoch second value**, not a raw NTP timestamp — lwIP does the
NTP-epoch-(1900)-to-Unix-epoch-(1970) conversion (and 2036
era-rollover handling) internally before invoking it. `rtc_sync.c`'s
job is purely the epoch-seconds-to-calendar breakdown from that point,
via ChibiOS's own `rtcConvertStructTmToDateTime()`/`rtcGetTime()`/
`rtcSetTime()` (`RTCD1`, the single RTC driver instance ChibiOS
declares for the F4).

**The `platform.h` bridge:** `platform_wallclock_unix_s()`
(`targets/stm32-e407/src/platform_chibios.c`) calls
`rtc_sync_unix_time()` only if `rtc_sync_is_synced()` is true;
otherwise it returns `0`. This is what makes an unsynced device report
an honest `0` (1970-01-01) in `?sensor-value` rather than whatever
arbitrary value the RTC powered up or last warm-reset with.

**A latent build bug this touched, worth knowing about:** ChibiOS's
`lwip.mk` defaults `LWSRC_EXTRAS` to the lwIP HTTP demo's source files
(`HTTPFILES`) unless overridden — which this project never set up (no
generated `fsdata.c` web content exists) and would likely have failed
to link the first time anyone actually tried building this target. The
Makefile now explicitly overrides it to `$(SNTPFILES)` instead, which
both fixes that and pulls in the SNTP client this section needed
anyway.

**Not implemented:** no sensor exposing sync status/staleness (e.g.
"seconds since last successful sync") even though
`rtc_sync_is_synced()` exists as a natural hook for one, and no
handling for a resync landing mid-request (a client polling
`?sensor-value` at the exact moment of a 6h resync could see a
timestamp step backward or forward — judged not worth engineering
around for a monitor-and-control sensor channel, but worth remembering
if that judgment call ever needs revisiting).

---

## 8. Testing without hardware

Because `common/src/katcp_proto.c`, `common/src/katcp_commands.c`, and
`common/src/sensor.c` have no vendor SDK/HAL dependency, they compile
and run as ordinary desktop C — the platform-specific pieces
`sensor.c` needs (a monotonic clock, and wall-clock time where
available) are supplied by `test/platform_host.c`
(`clock_gettime()`/`time()`-based) instead of any target's own
`platform_*.c`. `test/platform_host.c` is deliberately **not** listed
in any target's build files — it exists only for this desktop path.

```bash
# from the repo root
gcc -Wall -Wextra -std=c11 -Icommon/include -o test_harness \
    test_harness.c common/src/katcp_proto.c common/src/katcp_commands.c \
    test/synth_stub.c test/pac1954_stub.c common/src/sensor.c \
    test/platform_host.c
```

where `test_harness.c` is a few lines supplying a `katcp_out_fn` that
`fwrite()`s to stdout, and a string of test requests fed via
`katcp_feed()`. This is how the separator bug in §3.3 was caught, and
is worth extending as new commands are added rather than trusting
hand-inspection alone. Since `test/platform_host.c` uses the real host
clock, `?sensor-value`'s timestamp in this harness will show genuine
current Unix time, not a stand-in for `rtc_sync.c`'s SNTP behavior —
there's nothing to sync in a desktop test.

Each target's own transport glue (e.g.
`targets/stm32-e407/src/main.c`, the socket/DHCP/RTC code) has no
equivalent host test path — it depends on real vendor SDK/HAL headers
throughout, so it can only be exercised by actually building and
flashing.

Once running on real hardware, the same requests can be tried by hand
over the network:
```
$ nc 192.168.1.200 7147
?watchdog
!watchdog ok
```

---

## 9. Known gaps / suggested next steps

- **Synth driver** — the actual blocker to doing anything useful (§6).
- **`?halt` semantics** — currently just drops the connection; needs a
  project decision on what "halt" should actually do to the synth and
  the board itself.
- **`?sensor-sampling`** — not implemented. `?sensor-list`/`?sensor-value`
  (§4) are purely request/reply; there's no mechanism yet to push
  sensor updates to a client on a timer or on change. Would need new
  per-sensor sampling state and a scheduler driven from the main loop,
  distinct in kind from everything implemented so far (which is all
  request-driven).
- **No persistent DHCP retry** — by design, a one-shot attempt at boot
  with a timeout fallback (§5.1), not a background retry if a server
  appears later.
- **Single client / no interrupt-driven I/O** — see §5.2, §5.4; fine
  at current scale, worth reconsidering if usage patterns change.
- **No sensor exposing RTC sync status** — see §7.
- **PAC1954 untested on hardware** — I2C address, shunt values and
  default-range assumptions are placeholders (§6.4). Energy
  accumulator readout (`VACCn`) isn't exposed yet.
- **`mcu.temperature` has no `TS_CAL1` calibration** — see §6.2; a
  worthwhile follow-up once the calibration address/format is
  confirmed against ST's actual reference manual for this part.
- **`?synth-frequency` still stubbed** — see §6.3. Requires porting
  the actual frequency-to-register-set math (N-divider, VCO-curve
  selection, charge-pump-current lookup), not just implementing a
  formula; `?synth-write-register` (§4, §10) is the interim path for
  loading known-good configurations.

---

## 10. Julia client tooling (`julia/`)

Two pieces, deliberately separated: a general-purpose KATCP client
package with no project-specific knowledge, and a small script built
on it that knows about LMX2594-style register dumps specifically.
Building the general client first (rather than a one-off script) was
a deliberate call — the register-dump loader needs to speak the wire
protocol regardless, and writing that once as a reusable package costs
the same as a one-off script but doesn't need throwing away the moment
anything else KATCP-related is wanted from Julia (which, given this
project, was never realistically going to be a one-time need — even
just scripting `?sensor-value` polling would have wanted it).

### 10.1 `KatcpClient.jl` — general protocol client

```
julia/KatcpClient/
  Project.toml
  src/KatcpClient.jl
  test/runtests.jl
```

A standalone Julia package (own `Project.toml`, own UUID), not
registered anywhere — used via a relative path or `include()` for now
(see §10.2). Depends only on the `Sockets` stdlib.

**Escaping and tokenizing are separate, hand-ported implementations of
the same algorithms as `common/src/katcp_proto.c`** (§3.2, §3.3) —
not a shared codebase, since one side is C running on the STM32 and
the other is Julia running on a desktop/control machine. Every
mapping in `ESCAPE_OUT` corresponds line-for-line to the C escape
table. Verified in two stages before being trusted:
1. The algorithm was first prototyped and tested in Python (9 round-trip
   cases, 6 tokenize cases) purely to validate the logic, before any
   Julia was written, since no Julia runtime was available in the
   sandbox this was originally drafted in.
2. Once real Julia access became available, the actual `.jl` code was
   tested directly — `test/runtests.jl` re-runs the same escape/
   tokenize/parse cases against the real implementation (`Pkg.test()`
   passes, 40 assertions), plus three **live TCP loopback tests**
   against a fake KATCP server run in a background `@async` task on
   `localhost`, exercising `request()` (including informs arriving
   before a reply) and `request_pipeline()` with **deliberately
   out-of-order server replies** — confirming replies get matched back
   up by `[mid]` and returned in the original request order regardless
   of network arrival order, not just asserted to work.

**One real bug caught during this testing, worth recording:**
`abspath(PROGRAM_FILE) == @__FILE__ && main()` (the standard Julia
"run main() only if this file is executed directly, not `include`d"
idiom) is a **parse error** without parenthesizing the macro call —
`@__FILE__` used inline in a binary-operator chain creates a genuine
syntax ambiguity. Fixed by parenthesizing: `(@__FILE__)`. Not a
project-specific mistake so much as a sharp edge in Julia's macro-call
grammar worth remembering elsewhere.

**Core API:**

| Function | Purpose |
|---|---|
| `connect_katcp(host, port=7147)` | Opens a `KatcpConnection` |
| `request(conn, name, args...)` | Sends one request, blocks for its reply, returns `(reply, informs)` |
| `request_pipeline(conn, name, arglists)` | Sends many requests back-to-back, returns replies in `arglists` order regardless of reply arrival order (§10.1 above) |
| `check_ok(reply)` | Throws `KatcpRequestFailed` if `reply.status != "ok"`, otherwise returns it — optional, `request`/`request_pipeline` don't call this automatically |
| `escape_arg` / `unescape_arg` / `tokenize_line` / `parse_line` | The wire-format primitives, exported in case a caller needs them directly |

`request_pipeline` is the actual solution to "113 individual
request/reply round-trips would be slow": all requests are written to
the socket up front (matching how the server's tokenizer already
handles multiple complete requests arriving in one `recv()`, §3.3),
and replies are collected and re-sorted back into the caller's
original order by `[mid]`. Request *transmission* order (and therefore
the server's SPI *write* order) is still exactly the order given —
only reply *matching* is order-independent — which is what makes it
safe for LMX2594-style register dumps that require register 0 (VCO
calibration trigger) to be written last: passing the file's own order
straight through to `request_pipeline` preserves that requirement with
no special-casing needed.

### 10.2 `load_register_dump.jl` — the LMX2594-specific tool

```
julia/
  load_register_dump.jl
  test_fixtures/
    lmx2594_example.txt        -- the actual 113-register dump this was tested against
```

Loads a register-dump file via a plain `include()` of `KatcpClient.jl`
directly (not a formal package dependency — reasonable for a single
small co-located script; promoting it to a real `Project.toml`
dependency is a two-line change if this tooling grows).

**`parse_register_dump(path)`** parses lines like `R112\t0x700000`
into a `Vector{UInt32}`, preserving file order (see §10.1 on why that
matters), and **cross-checks each line's `R<n>` label against the
register address actually encoded in the value's own bits `[23:16]`**
— catching a hand-edited or corrupted dump file before it gets
anywhere near hardware. Tested against:
- The actual 113-register dump provided during this project's
  development (`test_fixtures/lmx2594_example.txt`) — correct count
  (113), correct first/last values (`R112`=`0x700000` first, `R0`=
  `0x00251C` last, confirming order preservation), and the
  label/address cross-check passing silently on all 113 real lines
  (would have errored otherwise).
- A deliberately corrupted 2-line fixture (mismatched label vs.
  encoded address) — confirmed to raise a precise, line-numbered error
  rather than silently pick one interpretation.

**`program_registers(host, words; port=7147)`** connects, replays
`words` via `request_pipeline(conn, "synth-write-register", ...)`, and
returns a `Bool` per register. Run end-to-end against a fake server
(mimicking the real firmware's exact reply format, including `[mid]`
tagging) using the real 113-register fixture: all 113 sent, all 113
replies matched back up correctly, all correctly reporting `fail` —
matching current firmware exactly, since `synth_write_raw_register()`
is still stubbed (§6). This is the strongest verification available
without real hardware: the full parse → connect → pipeline →
match-and-report path, exercised against the actual data, not
synthetic test values.

**Not yet run against real firmware** — no hardware to test against
yet (§6: no synth chip chosen, SPI not wired). First real test is
naturally whenever `synth_write_raw_register()` gets a real
implementation.

**Usage:**
```bash
julia julia/load_register_dump.jl <host> <hexfile> [port]
```

### 10.3 Validated against a real, independent KATCP server

Everything in §10.1's live-loopback tests exercises `KatcpClient.jl`
against a *fake* server written for the test itself — useful, but it
can't catch a case where this project's own assumptions about the
protocol and a real implementation's actual behavior diverge, since
the fake server was written under the same assumptions as the client.
So, read-only (no `?write`, `?progdev`, `?halt`, or any other
state-changing request), this was also tested against a genuine,
independent, unrelated KATCP server: a `tcpborphserver3` instance
(`katcp-library v0.2.0-304-gccea3d3-dirty`, an RFSoC-class CASPER
backend, normally driven by `casperfpga`) reachable on the local
network.

**This caught a real interop bug, not a hypothetical one.** The very
first `?watchdog[1]` request sent to it hung indefinitely —
`tcpborphserver3` replied with plain `!watchdog ok`, no `[mid]` at
all, which `request()`'s original strict `msg.mid == string(mid)`
match could structurally never satisfy. Root-caused by manually
reading raw lines off the socket with a bounded timeout rather than
guessing, which also surfaced something not otherwise obvious: the
server sends three unsolicited `#version-connect` informs immediately
on connect, before replying to anything. Fixed in `request()` (§10.1)
to tolerate a mid-less reply when it's the only request in flight;
`request_pipeline()` was deliberately left strict, since it has no
safe way to degrade when multiple requests are genuinely concurrent
(documented there as a real, known limitation if ever pointed at a
server like this one).

**After the fix, confirmed working end-to-end**, read-only, against
the real server: the full `#version-connect` connect banner parsed
correctly as informs; `?version-list` and `?help` (130 real commands
enumerated, including a large `rfdc-*` family for the RFSoC's RF Data
Converter subsystem); `?status`/`?fpgastatus`/`?listdev` correctly
reporting failure with `#fpga not programmed` informs (the board's
FPGA isn't currently loaded — a real, accurate device state, not a
client-side error); `?listbof` (`0`, consistent with the above); and
`?client-list` showing exactly one connected client — this session
itself, confirming a genuine live connection rather than a cached
response.

---

---

## 11. Web interface (`common/src/web_ui.c`, `targets/stm32-e407/src/web_server.c`)

A small HTTP server on the board itself, port 80: browse to
`http://<board-ip>/` for a page with a synth frequency input (in MHz)
and a live PAC1954 table (voltage / current / power per channel,
refreshed every second). It sits alongside KATCP rather than replacing
it — both run at once.

**Split, same as KATCP:** `web_ui.c` (in `common/`, no RTOS/lwIP
dependency) turns one request's text into response bytes through an
output callback, like `katcp_feed()`/`katcp_out_fn`. `web_server.c`
is the target glue: its own ChibiOS thread (`NORMALPRIO-1`, 3 KB
stack), blocking lwIP sockets, one connection at a time, read the
request head, call `web_ui_handle_request()`, close.

**Routes** (GET only; anything else is 405, unknown paths 404):

| Path | Returns |
|---|---|
| `/` | The page — self-contained HTML/CSS/JS, no external resources (the board may have no internet route) |
| `/api/status` | `{"synth":{"locked":bool,"last_set_hz":n},"pac1954":[{"channel":1,"voltage":v,"current":a,"power":w,"status":"nominal"},…]}` — values are `null` and status `"error"` when the channel has no fresh reading (§6.4) |
| `/api/frequency?hz=<n>` | Calls `synth_set_frequency(n)`; `{"ok":true,"hz":n}` or `{"ok":false,"error":"…"}` (400 if `hz` isn't 1–15 plain digits) |

Setting the frequency is a GET so the handler never has to parse a
request body. That's fine on a lab network, but any page the browser
loads could trigger it (CSRF), and there's no authentication at all —
same trust model as the KATCP port.

Sensor values are read through `sensor_table` by name, so the page
shows exactly what `?sensor-value` would. `mcu.temperature` is
deliberately **not** read from the HTTP thread: it drives ADC1
directly with no locking, and two threads converting at once would
race. `last_set_hz` only tracks frequencies set from the page;
`synth.h` has no getter, so a `?synth-frequency` over KATCP doesn't
show up there.

**Supporting changes:**
- `synth_lmx2594.c` now holds a mutex across each whole programming
  sequence (`synth_set_frequency()`'s register writes,
  `synth_write_raw_register()`). With two threads able to call it,
  `spiAcquireBus()` alone only kept individual 24-bit words intact,
  not a 13-register retune.
- `cfg/lwipopts.h`: `MEMP_NUM_NETCONN` and `MEMP_NUM_TCP_PCB` raised to
  8 (two listeners plus clients exceeded lwIP's defaults of 4/5), and
  `LWIP_SO_RCVTIMEO=1` so the HTTP thread can put a 2 s receive
  timeout on each client. Browsers open speculative connections they
  may never send on, and without the timeout one of those would block
  the single-connection server indefinitely.

**Verified:** the target builds cleanly (no warnings; `.text` grows by
about 7 KB). On the host, `web_ui_handle_request()` was driven both
directly and through a throwaway POSIX socket server, and the page was
loaded in headless Chromium against the §8 stubs: the table fills in,
the set-frequency round trip works (showing the stub synth's "failed"),
and there were no JS errors. **Not yet run on the board.**
