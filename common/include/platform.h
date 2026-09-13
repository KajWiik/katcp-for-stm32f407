#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* Milliseconds on some monotonic clock: time-since-boot on a real
 * target (e.g. targets/rp2040-w5500-pico/src/platform_pico.c), or
 * process-relative time on a desktop test build (test/platform_host.c).
 * Callers (sensor.c) only use this for timestamps and elapsed-time
 * comparisons -- never assume it means anything against wall-clock/UTC
 * time. Each target provides exactly one implementation of this
 * function, matching its own hardware/RTOS. */
uint64_t platform_now_ms(void);

#endif /* PLATFORM_H */
