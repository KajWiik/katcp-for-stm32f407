/*
 * Host-only implementation of platform_now_ms(), for compiling and
 * running common/src/{katcp_proto,katcp_commands,sensor}.c as plain
 * desktop C (see IMPLEMENTATION.md section 8). Not part of any target's
 * embedded build -- each target under targets/ provides its own
 * platform_*.c instead (e.g. targets/stm32-e407/src/platform_chibios.c).
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

uint32_t platform_wallclock_unix_s(void)
{
    /* The desktop host running this test build has a real clock and
     * no sync process to wait for, unlike the embedded targets --
     * just use it. Not a stand-in for rtc_sync.c's behavior (there's
     * nothing to test here re: SNTP/RTC), only for exercising
     * sensor.c's timestamp field with a plausible non-zero value. */
    return (uint32_t)time(NULL);
}
