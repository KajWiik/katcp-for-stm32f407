#include "platform.h"
#include "ch.h"

uint64_t platform_now_ms(void)
{
    /* systime_t is a 32-bit tick counter at CH_CFG_ST_FREQUENCY (10000
     * Hz here, see cfg/chconf.h) -- it wraps roughly every 4.3M ticks
     * / 10000 Hz \u2248 119 hours. TIME_I2MS() converts the raw tick delta,
     * so a single call here is fine; board.uptime (common/src/sensor.c)
     * will wrap on that same cadence, unlike the RP2040 target's
     * 64-bit to_ms_since_boot(). Acceptable for a control box; note it
     * if this ever needs to run unattended for many days. */
    return (uint64_t)TIME_I2MS(chVTGetSystemTimeX());
}
