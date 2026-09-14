#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* Milliseconds on some monotonic clock: time-since-boot on a real
 * target (e.g. targets/stm32-e407/src/platform_chibios.c), or
 * process-relative time on a desktop test build (test/platform_host.c).
 * Callers (sensor.c) only use this for timestamps and elapsed-time
 * comparisons -- never assume it means anything against wall-clock/UTC
 * time. Each target provides exactly one implementation of this
 * function, matching its own hardware/RTOS. */
uint64_t platform_now_ms(void);

/* Wall-clock Unix epoch seconds (UTC), for targets that have a real
 * time source. Returns 0 if this target has none, or hasn't
 * synced yet -- 0 (1970-01-01) is a self-evidently-not-real value,
 * used deliberately as the "unsynced" signal rather than a separate
 * status flag. targets/stm32-e407/src/platform_chibios.c implements
 * this for real, backed by rtc_sync.c's SNTP-synced RTC (see
 * IMPLEMENTATION.md §7); the desktop test build stubs it with real
 * host time (test/platform_host.c) since there's nothing to sync
 * there. Any future target with no wall-clock source should stub
 * this to return 0. */
uint32_t platform_wallclock_unix_s(void);

#endif /* PLATFORM_H */
