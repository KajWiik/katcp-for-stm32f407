# KATCP Server — Implementation Notes & Command Reference

Sections 1–4 and 6–8 describe the shared design in `common/` --
protocol, command handling, sensors, and the synth driver contract --
which is identical across every target in this repo. Section 5 covers
the hardware/transport layer for the one target currently built, the
W5500-EVB-Pico (RP2040 + hardwired W5500 Ethernet, bare-metal Pico
SDK). Other targets (e.g. `targets/stm32-e407`) will get their own
hardware-layer writeup once built -- either appended here as a new
section 5-equivalent, or in their own target README, whichever reads
better once there's real content to place. For toolchain setup and
build steps, see each target's own `README.md`
(e.g. `targets/rp2040-w5500-pico/README.md`).

---

## 1. Design goals and non-goals

**Goals:**
- Speak enough of the KATCP wire protocol to be a compliant, if
  minimal, device: correct framing, escaping, message IDs, and the
  request/reply/inform semantics a KATCP client library expects.
- Keep the protocol logic transport-agnostic so it can be tested on a
  desktop with plain `gcc`, with no Pico SDK, W5500, or hardware
  involved at all.
- Keep the synth driver behind a fixed, small interface so the choice
  of synthesizer IC doesn't ripple into the protocol or command layer.

**Explicit non-goals (for now):**
- The full KATCP core request set (`?sensor-sampling`, `?log-level`,
  client-side `?client-list`, etc.). Only the subset needed for this
  device is implemented — see §4.
- Multiple simultaneous client connections. One TCP socket, one client
  at a time (see §3.3).
- Retrying DHCP after the initial boot-time attempt. See §5.2 — this
  is a one-shot, timeout-and-fall-back design, not a persistent
  background retry.

---

## 2. File layout and responsibilities

```
common/                      -- shared across every target, no RTOS/HAL dependency
  include/
    katcp_proto.h      Wire protocol types and API (transport-agnostic)
    katcp_commands.h   Command table declaration + halt flag
    synth.h            Fixed contract for the synth driver
    sensor.h           Fixed contract for the sensor table + handlers
    platform.h         Monotonic-time abstraction (target vs. host builds)
  src/
    katcp_proto.c      Framing, escaping, tokenizing, dispatch
    katcp_commands.c   Command table and handler implementations
    synth_stub.c       Placeholder synth driver (always reports failure)
    sensor.c           Sensor table (synth.locked, board.uptime) + handlers
targets/
  rp2040-w5500-pico/        -- built; see §5 and its own README.md
    src/
      main.c           Pico SDK + W5500 glue: SPI setup, DHCP bring-up, TCP loop
      platform_pico.c  platform_now_ms() via Pico SDK's to_ms_since_boot()
    CMakeLists.txt     Pico SDK + ioLibrary_Driver build configuration
  stm32-e407/               -- planned; see its own README.md
test/
  platform_host.c      platform_now_ms() via clock_gettime() -- desktop
                       tests only, not part of any target's build (see §7)
```

The dependency direction is strict and one-way:

```
main.c  --->  katcp_commands.c  --->  katcp_proto.c
  |                  |         \
  v                  v          --->  sensor.c ---> platform.h
W5500 / SPI       synth.h  --->  synth_stub.c (or a real driver later)
```

`katcp_proto.c` never includes anything from the Pico SDK, WIZnet's
`ioLibrary_Driver`, `synth.h`, or `sensor.h`. It only knows about bytes
in (`katcp_feed`) and a caller-supplied output callback
(`katcp_out_fn`). `sensor.c` follows the same discipline via
`platform.h`: it calls `platform_now_ms()` for timestamps rather than
any Pico-specific time API directly, which is what keeps it — like
the protocol and command layers — buildable and testable as plain
desktop C (§7).

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
> test harness (§7), not by inspection, which is why that harness is
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

All six requests currently implemented, in `katcp_commands.c`. Every
handler replies exactly once, as KATCP requires.

### `?halt`
Halts the device.
- **Args:** none
- **Reply:** `!halt ok`
- **Side effect:** sets `katcp_halt_requested = 1`. `main.c` checks this
  flag after each `katcp_feed()` call; the current implementation just
  disconnects the client (see §5.4) — it does **not** reset the RP2040
  or park the synth in a safe state. That's a deliberate placeholder:
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
  !help ok 6
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
  — `min`/`max` are only appended for `integer`-typed sensors
- **With argument:** single matching inform + `!sensor-list ok 1`, or
  `!sensor-list fail unknown sensor`
- **Currently defined sensors:**

  | Name | Type | Units | Range | Backed by |
  |---|---|---|---|---|
  | `synth.locked` | boolean | *(none)* | 0–1 | `synth_is_locked()` |
  | `board.uptime` | integer | seconds | 0–INT64_MAX | `platform_now_ms()/1000` |

- **Example:**
  ```
  ?sensor-list
  #sensor-list synth.locked Synthesizer PLL lock status \@ boolean
  #sensor-list board.uptime Seconds since boot seconds integer 0 9223372036854775807
  !sensor-list ok 2
  ```
  (The `\@` is the empty-argument marker from §3.2 — `synth.locked` has
  no units, so an empty argument is correctly escaped rather than
  silently dropped.)

### `?sensor-value [sensor-name]`
Reports current sensor value(s).
- **No argument:** one `#sensor-value` inform per sensor, then
  `!sensor-value ok <count>`
- **Inform format:** `#sensor-value <timestamp-ms> <count> <name> <status> <value>`
  — `count` is always `1` here (this implementation reports one
  sensor per inform rather than batching several under one
  timestamp, which is a KATCP-legal simplification)
- **Timestamp:** integer milliseconds from `platform_now_ms()` — see
  §6.1 for why this is deliberately not a `float` KATCP timestamp
- **Status word:** `nominal`, `warn`, `error`, or `unknown` — both
  current sensors always report `nominal` (the read itself can't fail;
  see the comment in `sensor.c` on `read_synth_locked` for why an
  *unlocked* synth is reported as a nominal `0` value, not a sensor
  error)
- **With argument:** single sensor's value + `!sensor-value ok 1`, or
  `!sensor-value fail unknown sensor`
- **Example:**
  ```
  ?sensor-value
  #sensor-value 136118 1 synth.locked nominal 0
  #sensor-value 136118 1 board.uptime nominal 136
  !sensor-value ok 2
  ```

> **Not implemented:** `?sensor-sampling` (periodic/async sensor
> push). Both commands above are purely request/reply — nothing is
> pushed to the client without being asked. See §8.

---

## 5. RP2040/W5500-EVB-Pico transport layer (`targets/rp2040-w5500-pico/src/main.c`)

### 5.1 Pin assignment

Fixed by the W5500-EVB-Pico's on-board wiring (SPI0), not a firmware
choice:

| Signal | GPIO |
|---|---|
| MISO | 16 |
| CS | 17 |
| SCK | 18 |
| MOSI | 19 |
| RST | 20 |
| INT | 21 (wired but currently unused — polling instead, see §5.3) |

SPI clock is initialized at 20 MHz (`spi_init(SPI_PORT, 20*1000*1000)`)
— conservative relative to the W5500's ~80 MHz ceiling, intended as a
safe starting point rather than a measured maximum.

### 5.2 Network configuration

Two-stage: DHCP first, static fallback.

1. `spi_and_wizchip_init()` programs `net_info` (the static config,
   hardcoded in `main.c`) onto the chip immediately, so the device
   always has *some* valid configuration from the moment
   initialization completes.
2. `dhcp_bringup()` then runs, once, blocking, at startup: it drives
   `ioLibrary_Driver`'s DHCP client for up to `DHCP_TIMEOUT_S`
   (default **30**, overridable at build time with
   `-DDHCP_TIMEOUT_S=<n>`) seconds.
   - **Lease obtained in time:** `net_info` is overwritten with the
     leased IP/gateway/subnet/DNS, `wizchip_setnetinfo()` is called
     again with the new values, and `net_info.dhcp` is updated to
     `NETINFO_DHCP`.
   - **Timeout, or `DHCP_run()` reports `DHCP_FAILED`:** `DHCP_stop()`
     is called and the function returns — `net_info` is simply never
     touched again, so the static config from step 1 remains active
     on the chip exactly as programmed. There is no separate
     "fall back" code path; not overwriting *is* the fallback.

**This is a boot-time decision only.** If no DHCP server is present at
startup, the device runs on the static config for its entire uptime,
even if a server appears later — `dhcp_bringup()` is never called
again automatically. Re-attempting DHCP later (e.g. from a
`?net-renew`-style command) is a natural extension but not implemented.

The static fallback config (`net_info` in `main.c`) still must be
changed per deployment, and the placeholder MAC
(`02:00:00:00:00:01`, locally-administered) must be unique per unit —
this matters for DHCP too, since servers key leases on the MAC.

DHCP uses hardware socket 7 (`DHCP_SOCKET`), separate from the KATCP
listener's socket 0, so the two don't contend — and DHCP's socket is
only active during the boot-time `dhcp_bringup()` call, freeing it up
implicitly once that returns (nothing currently reclaims socket 7 for
other use afterward, but nothing needs to yet either).

### 5.3 Socket handling

One TCP socket (`W5500` socket 0), listening on port **7147** (the
conventional KATCP/`tcpborphserver` port). The main loop polls
`getSn_SR()` and drives a small explicit state machine:

| Socket status | Action |
|---|---|
| `SOCK_CLOSED` | Re-open the socket (`socket()`) |
| `SOCK_INIT` | `listen()` |
| `SOCK_ESTABLISHED` | Drain `Sn_IR_CON`; if bytes are available (`getSn_RX_RSR`), `recv()` up to `sizeof(rx_scratch)` (512 B) and feed them to `katcp_feed()`; if `katcp_halt_requested`, disconnect |
| `SOCK_CLOSE_WAIT` | `disconnect()` |

This is polling, not interrupt-driven (the `INT` pin is wired to
GPIO21 but not currently used) — acceptable for a low-rate control
protocol, but worth revisiting if you later want the main loop free to
do something else (e.g. a periodic sensor poll) between polls.

### 5.4 Output path

`katcp_out_to_socket()` is the `katcp_out_fn` callback: every reply or
inform line the protocol layer produces is sent immediately via a
single `send()` call. There is no output buffering/batching — a
`?help` with many informs currently does one `send()` per line rather
than coalescing them into fewer TCP segments. Fine at this command
rate; worth revisiting only if `?help`-sized bursts ever become a
bottleneck.

### 5.5 One client at a time

Only socket 0 is opened. A second client connecting while one is
already `SOCK_ESTABLISHED` will simply not be serviced (the state
machine only drives one socket). KATCP itself permits multiple
concurrent clients; this implementation doesn't yet, which is fine for
a single-controller setup but worth knowing if two systems ever expect
to talk to this box at once.

---

## 6. Synth driver contract (`common/include/synth.h` / `common/src/synth_stub.c`)

### 6.1 Why integers, not floats

Both the synth driver contract and the sensor table (§4) deliberately
avoid `float`/`double` anywhere in the wire-facing code: the RP2040's
Cortex-M0+ has no hardware FPU, and `printf`-style `%f` formatting is
not reliably linked in by the newlib-nano C library configuration the
Pico SDK toolchain typically uses — it can silently emit garbage or
fail to link depending on build flags, which is a bad failure mode to
discover on already-deployed hardware. `?synth-frequency` uses integer
Hz; sensor timestamps are integer milliseconds; `board.uptime` is
integer seconds. If a genuinely fractional value is ever needed,
prefer a scaled integer (e.g. millidegrees, or Hz instead of GHz) over
introducing a float type.

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
needs to change.

---

## 7. Testing without hardware

Because `common/src/katcp_proto.c`, `common/src/katcp_commands.c`, and
`common/src/sensor.c` have no Pico SDK or W5500 dependency, they
compile and run as ordinary desktop C — the one thing `sensor.c` needs
from the platform (a monotonic clock, for timestamps) is supplied by
`test/platform_host.c` (`clock_gettime()`-based) instead of any
target's own `platform_*.c` (e.g. `targets/rp2040-w5500-pico/src/platform_pico.c`,
`to_ms_since_boot()`-based). `test/platform_host.c` is deliberately
**not** listed in any target's `CMakeLists.txt`/build files — it
exists only for this desktop path, shared by every target.

```bash
# from the repo root
gcc -Wall -Wextra -std=c11 -Icommon/include -o test_harness \
    test_harness.c common/src/katcp_proto.c common/src/katcp_commands.c \
    common/src/synth_stub.c common/src/sensor.c test/platform_host.c
```

where `test_harness.c` is a few lines supplying a `katcp_out_fn` that
`fwrite()`s to stdout, and a string of test requests fed via
`katcp_feed()`. This is how the separator bug in §3.3 was caught, and
is worth extending as new commands are added rather than trusting
hand-inspection alone.

Each target's own transport glue (e.g. `targets/rp2040-w5500-pico/src/main.c`,
the SPI/socket/DHCP code) has no equivalent host test path — it depends
on real vendor SDK/HAL headers throughout, so it can only be exercised
by actually building and flashing that target.

Once running on real hardware, the same requests can be tried by hand
over the network:
```
$ nc 192.168.1.200 7147
?watchdog
!watchdog ok
```

---

## 8. Known gaps / suggested next steps

- **Synth driver** — the actual blocker to doing anything useful (§6).
- **`?halt` semantics** — currently just drops the connection; needs a
  project decision on what "halt" should actually do to the synth and
  the RP2040 itself.
- **`?sensor-sampling`** — not implemented. `?sensor-list`/`?sensor-value`
  (§4) are purely request/reply; there's no mechanism yet to push
  sensor updates to a client on a timer or on change. Would need new
  per-sensor sampling state and a scheduler driven from the main loop,
  distinct in kind from everything implemented so far (which is all
  request-driven).
- **No persistent DHCP retry** — by design, a one-shot attempt at boot
  with a timeout fallback (§5.2), not a background retry if a server
  appears later.
- **Single client / polling I/O** — see §5.3, §5.5; fine at current
  scale, worth reconsidering if usage patterns change.
