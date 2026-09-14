#include "platform.h"
#include "ch.h"
#include "rtc_sync.h"

uint64_t platform_now_ms(void)
{
    /* systime_t is a 32-bit tick counter at CH_CFG_ST_FREQUENCY (10000
     * Hz here, see cfg/chconf.h) -- it wraps roughly every 4.3M ticks
     * / 10000 Hz ≈ 119 hours. TIME_I2MS() converts the raw tick delta,
     * so a single call here is fine; board.uptime (common/src/sensor.c)
     * will wrap on that same cadence. Acceptable for a control box,
     * worth noting if this ever needs to run unattended for many days. */
    return (uint64_t)TIME_I2MS(chVTGetSystemTimeX());
}

uint32_t platform_wallclock_unix_s(void)
{
    /* rtc_sync_unix_time() reads the RTC unconditionally, including
     * before any SNTP sync has happened -- gate it on
     * rtc_sync_is_synced() so an unsynced device reports 0 (the
     * documented "unsynced" signal in platform.h) rather than
     * whatever arbitrary value the RTC powered up or last warm-reset
     * with. */
    if (!rtc_sync_is_synced()) {
        return 0;
    }
    return rtc_sync_unix_time();
}
