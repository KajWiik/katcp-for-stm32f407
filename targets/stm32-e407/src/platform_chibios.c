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

double platform_mcu_temperature_c(void)
{
    /* Group config modeled directly on ChibiOS's own STM32F4xx ADC
     * testhal example (testhal/STM32/STM32F4xx/ADC/main.c), which
     * demonstrates this exact internal channel -- ADC_SAMPLE_144 for
     * the sensor channel is theirs, not derived from scratch. No GPIO
     * setup needed: channel 16 (ADC_CHANNEL_SENSOR) is purely
     * internal, unlike a regular ANx pin. */
    static const ADCConversionGroup grpcfg = {
        .circular     = false,
        .num_channels = 1,
        .end_cb       = NULL,
        .error_cb     = NULL,
        .cr1          = 0,
        .cr2          = ADC_CR2_SWSTART,
        .smpr1        = ADC_SMPR1_SMP_SENSOR(ADC_SAMPLE_144),
        .smpr2        = 0,
        .htr          = 0,
        .ltr          = 0,
        .sqr1         = 0,
        .sqr2         = 0,
        .sqr3         = ADC_SQR3_SQ1_N(ADC_CHANNEL_SENSOR),
    };
    adcsample_t sample;

    adcSTM32EnableTSVREFE();
    adcStart(&ADCD1, NULL);
    adcConvert(&ADCD1, &grpcfg, &sample, 1);
    adcStop(&ADCD1);
    adcSTM32DisableTSVREFE();

    /* Datasheet TYPICAL values, NOT chip-calibrated (see platform.h
     * and IMPLEMENTATION.md §6.2 for why: no verified TS_CAL1
     * factory-calibration address/format was confirmed for this
     * specific part, so this doesn't guess at one). V25 = 0.76V,
     * Avg_Slope = 2.5mV/°C, Vref+ assumed 3.3V nominal (not read from
     * VREFINT), 12-bit ADC (4095 full scale).
     *
     * Real double arithmetic now (see sensor.h/platform.h on the FPU
     * reversal) -- this is measurably simpler to read than the old
     * scaled-integer version was, which is itself a small point in
     * favor of the change beyond just the synth math it was mainly
     * made for. */
    double v_sense_v = (double)sample * 3.3 / 4095.0;
    double temp_c     = (v_sense_v - 0.76) / 0.0025 + 25.0;

    return temp_c;
}
