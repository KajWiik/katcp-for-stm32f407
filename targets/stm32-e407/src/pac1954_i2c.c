/*
 * pac1954_i2c.c -- pac1954.h driver for the Microchip PAC1954 4-channel
 * power monitor, over I2C1 on the Olimex STM32-E407 (ChibiOS). Target-
 * specific (ChibiOS HAL), so it lives here next to synth_lmx2594.c;
 * all code-to-physical-unit math stays in common/ (pac1954.h,
 * sensor.c), this file only moves raw register codes
 * (IMPLEMENTATION.md §6.4).
 *
 * A low-priority polling thread does the actual I2C traffic and keeps
 * a mutex-protected snapshot, so pac1954_read_raw() -- called from the
 * KATCP thread via sensor.c -- never blocks on the bus, as sensor.h's
 * "must not block" contract asks. Each poll:
 *   1. REFRESH_V (0x1F): latch the latest conversions into the readable
 *      registers WITHOUT resetting the accumulators (plain REFRESH,
 *      0x00, would reset them -- not wanted, in case accumulator/energy
 *      readout is ever added).
 *   2. Wait > 1 ms (the datasheet's minimum before reading refreshed data).
 *   3. Read VBUSn_AVG / VSENSEn_AVG for all four channels -- the
 *      chip's rolling average of the last 8 conversions, less noisy
 *      than the single-sample VBUSn/VSENSEn registers.
 * Each register is read with its own 2-byte transaction rather than
 * one auto-incrementing block read: block reads on this chip family
 * skip registers of disabled channels, which would silently shift
 * every following value if a channel were ever turned off in CTRL.
 *
 * ============================================================
 * CONFIGURE THESE FOR YOUR BOARD
 * ============================================================
 *
 * I2C: I2C1 on PB8 (SCL) / PB9 (SDA), which the ChibiOS
 * OLIMEX_STM32_E407_REV_D board file already sets up as AF4 open-drain
 * (it's the E407's UEXT connector I2C) -- no palSetLineMode() needed
 * here. External pull-ups are required as usual.
 *
 * 7-bit address: set by the PAC1954's ADDRSEL pin strap (resistor to
 * GND, see the datasheet's address-selection table). 0x10 is a
 * placeholder, not confirmed against any real board.
 */

#include "ch.h"
#include "hal.h"
#include "pac1954.h"

/* ---- CONFIGURE: bus and address (see header comment above) -------- */
#define PAC_I2CD          (&I2CD1)
#define PAC_I2C_ADDR      0x10U
#define PAC_POLL_MS       100U
/* A snapshot older than this is reported as unavailable (sensor
 * status "error"), so a chip that drops off the bus mid-run doesn't
 * keep reporting its last good value forever. */
#define PAC_STALE_MS      1000U

/* ---- register map (PAC195x datasheet) ----------------------------- */
#define PAC_REG_VBUS1_AVG    0x0FU   /* ..0x12 for channels 1-4 */
#define PAC_REG_VSENSE1_AVG  0x13U   /* ..0x16 for channels 1-4 */
#define PAC_REG_REFRESH_V    0x1FU

#define PAC_I2C_TIMEOUT   TIME_MS2I(10)

static const I2CConfig pac_i2ccfg = {
    .op_mode     = OPMODE_I2C,
    .clock_speed = 400000,
    .duty_cycle  = FAST_DUTY_CYCLE_2,
};

static mutex_t  pac_mtx;
static uint16_t pac_vbus[PAC1954_NUM_CHANNELS];
static uint16_t pac_vsense[PAC1954_NUM_CHANNELS];
static bool     pac_have_snapshot;
static systime_t pac_snapshot_time;

static msg_t pac_send_command(uint8_t reg)
{
    return i2cMasterTransmitTimeout(PAC_I2CD, PAC_I2C_ADDR, &reg, 1,
                                    NULL, 0, PAC_I2C_TIMEOUT);
}

static msg_t pac_read_u16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    msg_t r = i2cMasterTransmitTimeout(PAC_I2CD, PAC_I2C_ADDR, &reg, 1,
                                       rx, 2, PAC_I2C_TIMEOUT);
    if (r == MSG_OK) {
        *out = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]); /* MSB first */
    }
    return r;
}

/* One full refresh + read of all channels. On any failure nothing is
 * published, so a snapshot never mixes values from different refreshes. */
static bool pac_poll_once(void)
{
    uint16_t vbus[PAC1954_NUM_CHANNELS], vsense[PAC1954_NUM_CHANNELS];
    msg_t r;

    i2cAcquireBus(PAC_I2CD);
    r = pac_send_command(PAC_REG_REFRESH_V);
    if (r == MSG_OK) {
        i2cReleaseBus(PAC_I2CD);
        chThdSleepMilliseconds(2);
        i2cAcquireBus(PAC_I2CD);
    }
    for (int ch = 0; r == MSG_OK && ch < PAC1954_NUM_CHANNELS; ch++) {
        r = pac_read_u16((uint8_t)(PAC_REG_VBUS1_AVG + ch), &vbus[ch]);
        if (r == MSG_OK) {
            r = pac_read_u16((uint8_t)(PAC_REG_VSENSE1_AVG + ch), &vsense[ch]);
        }
    }
    if (r == MSG_TIMEOUT) {
        /* Per ChibiOS's hal_i2c docs, a timeout leaves the driver in an
         * unusable state -- it has to be stopped and restarted before
         * the next transfer. */
        i2cStop(PAC_I2CD);
        i2cStart(PAC_I2CD, &pac_i2ccfg);
    }
    i2cReleaseBus(PAC_I2CD);

    if (r != MSG_OK) {
        return false;
    }

    chMtxLock(&pac_mtx);
    for (int ch = 0; ch < PAC1954_NUM_CHANNELS; ch++) {
        pac_vbus[ch]   = vbus[ch];
        pac_vsense[ch] = vsense[ch];
    }
    pac_have_snapshot = true;
    pac_snapshot_time = chVTGetSystemTimeX();
    chMtxUnlock(&pac_mtx);
    return true;
}

static THD_WORKING_AREA(wa_pac_thread, 512);
static THD_FUNCTION(pac_thread, arg)
{
    (void)arg;
    chRegSetThreadName("pac1954");

    for (;;) {
        (void)pac_poll_once();
        chThdSleepMilliseconds(PAC_POLL_MS);
    }
}

/* ==================================================================
 * pac1954.h contract
 * ================================================================== */

void pac1954_init(void)
{
    chMtxObjectInit(&pac_mtx);
    i2cStart(PAC_I2CD, &pac_i2ccfg);
    /* Chip is left at its power-on defaults (1024 SPS adaptive
     * accumulation, all four channels enabled, unipolar ranges) --
     * pac1954.h's conversion helpers assume exactly that. */
    chThdCreateStatic(wa_pac_thread, sizeof(wa_pac_thread),
                      NORMALPRIO - 1, pac_thread, NULL);
}

bool pac1954_read_raw(int ch, uint16_t *vbus, uint16_t *vsense)
{
    if (ch < 0 || ch >= PAC1954_NUM_CHANNELS) {
        return false;
    }

    bool ok;
    chMtxLock(&pac_mtx);
    ok = pac_have_snapshot &&
         chVTTimeElapsedSinceX(pac_snapshot_time) < TIME_MS2I(PAC_STALE_MS);
    if (ok) {
        *vbus   = pac_vbus[ch];
        *vsense = pac_vsense[ch];
    }
    chMtxUnlock(&pac_mtx);
    return ok;
}
