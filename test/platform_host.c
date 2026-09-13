/*
 * Host-only implementation of platform_now_ms(), for compiling and
 * running common/src/{katcp_proto,katcp_commands,sensor}.c as plain
 * desktop C (see IMPLEMENTATION.md section 7). Not part of any target's
 * embedded build -- each target under targets/ provides its own
 * platform_*.c instead (e.g. targets/rp2040-w5500-pico/src/platform_pico.c).
 */
#define _POSIX_C_SOURCE 199309L
#include "platform.h"
#include <time.h>

uint64_t platform_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}
