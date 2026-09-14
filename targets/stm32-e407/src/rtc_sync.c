/*
 * SNTP-based RTC sync. See rtc_sync.h and IMPLEMENTATION.md section 7.
 *
 * The actual "boot sync + resync every 6 hours" scheduling is entirely
 * lwIP's own SNTP module (see SNTP_UPDATE_DELAY in cfg/lwipopts.h) --
 * this file only supplies the callback it invokes on each successful
 * sync (rtc_sync_on_sntp_time) and a way to read the result back
 * (rtc_sync_unix_time).
 */

#include "rtc_sync.h"

#include <string.h>
#include <time.h>

#include "hal.h"
#include "lwip/ip_addr.h"   /* ip_addr_copy_from_ip4() -- don't rely on it coming in transitively */
#include "lwip/apps/sntp.h"

static volatile bool synced = false;

/* ---- Unix epoch <-> UTC calendar, self-contained --------------------------
 * Deliberately not using libc's gmtime()/mktime(): this keeps UTC
 * handling fully deterministic without depending on newlib's timezone
 * setup being correct in this embedded configuration. Public-domain
 * algorithm by Howard Hinnant:
 * http://howardhinnant.github.io/date_algorithms.html#civil_from_days
 * Verified against glibc's gmtime() across the epoch, leap-day,
 * signed-32-bit-overflow (2038), and uint32_t-max (2106) boundaries
 * before being used here. */

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);              /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    int64_t yr = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);    /* [0, 365] */
    unsigned mp = (5 * doy + 2) / 153;                          /* [0, 11] */
    *d = doy - (153 * mp + 2) / 5 + 1;                           /* [1, 31] */
    *m = mp + (mp < 10 ? 3 : (unsigned)-9);                      /* [1, 12] */
    *y = (int)(yr + (*m <= 2));
}

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void unix_to_tm_utc(uint32_t epoch, struct tm *tm)
{
    int64_t days = (int64_t)(epoch / 86400U);
    uint32_t rem = epoch % 86400U;

    memset(tm, 0, sizeof(*tm));
    tm->tm_hour = (int)(rem / 3600U);
    rem %= 3600U;
    tm->tm_min = (int)(rem / 60U);
    tm->tm_sec = (int)(rem % 60U);

    int y;
    unsigned mo, d;
    civil_from_days(days, &y, &mo, &d);
    tm->tm_year = y - 1900;
    tm->tm_mon  = (int)mo - 1;
    tm->tm_mday = (int)d;

    /* 1970-01-01 (days == 0) was a Thursday. */
    tm->tm_wday = (int)(((days % 7) + 7 + 4) % 7);
    tm->tm_isdst = 0;
}

/* ---- lwIP SNTP callback -- see SNTP_SET_SYSTEM_TIME in cfg/lwipopts.h ----
 * lwIP has already converted the wire NTP timestamp to a Unix epoch
 * second value by the time this is called (see sntp.c's
 * SNTP_SET_SYSTEM_TIME_NTP macro chain) -- no NTP-epoch-to-Unix-epoch
 * math needed here, only calendar breakdown. */
void rtc_sync_on_sntp_time(uint32_t unix_time)
{
    struct tm tm;
    RTCDateTime dt;

    unix_to_tm_utc(unix_time, &tm);
    rtcConvertStructTmToDateTime(&tm, 0, &dt);
    rtcSetTime(&RTCD1, &dt);
    synced = true;
}

void rtc_sync_start(const ip4_addr_t *server_ip)
{
    ip_addr_t addr;
    ip_addr_copy_from_ip4(addr, *server_ip);
    sntp_setserver(0, &addr);
    sntp_init(); /* First request fires shortly after this call; every
                  * 6h resync after that is handled internally by
                  * lwIP (SNTP_UPDATE_DELAY, cfg/lwipopts.h) -- no
                  * timer/loop of our own needed. */
}

uint32_t rtc_sync_unix_time(void)
{
    RTCDateTime dt;
    struct tm tm;
    uint32_t ms;

    rtcGetTime(&RTCD1, &dt);
    rtcConvertDateTimeToStructTm(&dt, &tm, &ms);

    int64_t days = days_from_civil(tm.tm_year + 1900,
                                    (unsigned)(tm.tm_mon + 1),
                                    (unsigned)tm.tm_mday);
    return (uint32_t)(days * 86400 + tm.tm_hour * 3600 +
                       tm.tm_min * 60 + tm.tm_sec);
}

bool rtc_sync_is_synced(void)
{
    return synced;
}
