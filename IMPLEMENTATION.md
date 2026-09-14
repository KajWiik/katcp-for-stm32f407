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
    sensor.h           Fixed contract for the sensor table + handlers
    platform.h         Monotonic + wall-clock time abstraction (target vs. host builds)
  src/
    katcp_proto.c      Framing, escaping, tokenizing, dispatch
    katcp_commands.c   Command table and handler implementations
    synth_stub.c       Placeholder synth driver (always reports failure)
    sensor.c           Sensor table (synth.locked, board.uptime) + handlers
targets/
  stm32-e407/               -- see §5 and its own README.md
    src/
      main.c           ChibiOS/lwIP glue: network bring-up, KATCP-over-socket thread
      platform_chibios.c  platform.h implementation (RTC-backed wall clock)
      rtc_sync.c       SNTP-based RTC sync (§7)
      portab.c/.h      Minimal board hook (LED heartbeat)
    cfg/               chconf.h / halconf.h / mcuconf.h / lwipopts.h
    Makefile
test/
  platform_host.c      platform.h implementation for desktop tests (§8)
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

All eight requests currently implemented, in `katcp_commands.c`. Every
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
  #help sensor-list List available sensors, or describe one.
  #help sensor-value Report current sensor value(s).
  !help ok 8
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
- **Status word:** `nominal`, `warn`, `error`, or `unknown` — both
  current sensors always report `nominal` (the read itself can't fail;
  see the comment in `sensor.c` on `read_synth_locked` for why an
  *unlocked* synth is reported as a nominal `0` value, not a sensor
  error)
- **With argument:** single sensor's value + `!sensor-value ok 1`, or
  `!sensor-value fail unknown sensor`
- **Example** (from a live run, with the RTC already synced):
  ```
  ?sensor-value
  #sensor-value 1789413904 1 synth.locked nominal 0
  #sensor-value 1789413904 1 board.uptime nominal 35
  !sensor-value ok 2
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

---

## 6. Synth driver contract (`common/include/synth.h` / `common/src/synth_stub.c`)

### 6.1 Why integers, not floats

Both the synth driver contract and the sensor table (§4) deliberately
avoid `float`/`double` anywhere in the wire-facing code. The STM32F407
does have a hardware FPU (Cortex-M4F), but this project doesn't use it
(`USE_FPU = no` in the Makefile) — the reasons are independent of
whether an FPU is present: `printf`-style `%f` formatting isn't
reliably linked in by the newlib-nano C library configuration common
in embedded toolchains (it can silently emit garbage or fail to link
depending on build flags), and integer arithmetic keeps behavior
simple and deterministic regardless of chip. `?synth-frequency` uses
integer Hz; sensor timestamps are integer Unix seconds; `board.uptime`
is integer seconds. If a genuinely fractional value is ever needed,
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
needs to change. SPI itself isn't even enabled yet in
`targets/stm32-e407/cfg/mcuconf.h`/`halconf.h` — deliberately deferred
until the chip and its wiring to this board are chosen.

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
    common/src/synth_stub.c common/src/sensor.c test/platform_host.c
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
