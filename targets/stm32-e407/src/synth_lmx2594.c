/*
 * synth_lmx2594.c -- real synth.h driver for the TI LMX2594, on the
 * Olimex STM32-E407 (ChibiOS), replacing common/src/synth_stub.c for
 * this target (IMPLEMENTATION.md §6, §9). This is target-specific
 * (ChibiOS HAL) so it lives here, next to platform_chibios.c, rather
 * than in common/src/ -- common/ stays free of RTOS/HAL dependencies,
 * same discipline as katcp_proto.c/katcp_commands.c/sensor.c.
 *
 * Two distinct problems, both solved here (IMPLEMENTATION.md §6.3):
 *   - ?synth-write-register (already implemented at the KATCP layer)
 *     now actually reaches SPI via synth_write_raw_register() below --
 *     a raw, unopinionated 24-bit word write, exactly as documented.
 *   - ?synth-frequency now computes a full register set for an
 *     arbitrary target frequency: N-divider, channel-divider (Table 8),
 *     VCO core select, and -- the part that was the actual point of
 *     the exercise -- charge-pump current via the loop-gain header
 *     (lmx2594_loop_gain_fp.h) so loop bandwidth stays roughly
 *     constant across VCO cores for one fixed physical loop filter.
 *
 * ============================================================
 * CONFIGURE THESE FOR YOUR BOARD -- everything in this block is a
 * placeholder until the real wiring/reference design is confirmed.
 * ============================================================
 *
 * SPI: SPI1, Arduino-compatible header (as given 2026-09-15):
 *   SCK  = PA5 (D13)   MISO = PA6 (D12)
 *   MOSI = PB5 (D11)   CS   = PA4 (D10)  -- synth 1
 * Two LMX2594s share this bus (SYNTH_COUNT in synth.h), one CS each.
 * Synth 2's CS is PB6 -- a PLACEHOLDER, picked only because the
 * ChibiOS board file leaves it unused; not confirmed against the real
 * wiring. Both synths are assumed to share the same reference/fPD and
 * baseline register template below.
 * MUXOUT/lock-detect: not yet wired -- synth_is_locked() returns
 * false unconditionally until LMX_MUXOUT_WIRED is set and a real
 * line substituted in.
 *
 * Reference/PFD frequency: 200 MHz (as given 2026-09-15), assumed
 * equal to fOSCin (doubler off, R=1/no pre-divider) -- consistent
 * with the baseline register template below, which was NOT captured
 * against this specific board (see the big comment above
 * lmx_baseline_regs[]). If OSC_2X/PLL_R/PLL_R_PRE ever change from
 * that template, LMX_FPD_HZ must be updated to match, or the whole
 * frequency computation is wrong by a fixed, silent scale factor.
 */

#include "hal.h"
#include "synth.h"
#include "lmx2594_loop_gain_fp.h"

#include <string.h>

/* ---- CONFIGURE: SPI pins (see header comment above) -------------- */
#define LMX_SPID          (&SPID1)
static const ioline_t lmx_cs_lines[SYNTH_COUNT] = {
    PAL_LINE(GPIOA, 4U),   /* synth 1 */
    PAL_LINE(GPIOB, 6U),   /* synth 2 -- PLACEHOLDER, see header comment */
};
#define LMX_SCK_LINE      PAL_LINE(GPIOA, 5U)
#define LMX_MISO_LINE     PAL_LINE(GPIOA, 6U)
#define LMX_MOSI_LINE     PAL_LINE(GPIOB, 5U)
#define LMX_SPI_AF        5U   /* SPI1 alternate function on the F407 */

/* ---- CONFIGURE: MUXOUT / lock-detect (not yet wired) -------------- */
#define LMX_MUXOUT_WIRED  0
#if LMX_MUXOUT_WIRED
static const ioline_t lmx_muxout_lines[SYNTH_COUNT] = {
    PAL_LINE(GPIOx, N),   /* synth 1 -- fill in once wired */
    PAL_LINE(GPIOx, N),   /* synth 2 -- fill in once wired */
};
#endif

/* ---- CONFIGURE: reference / phase-detector frequency --------------
 * See the big header comment: this is the single most likely source
 * of "it locks, but to the wrong frequency" if it's wrong. */
#define LMX_FPD_HZ        200000000.0f

/* Serializes whole programming sequences: both the KATCP thread and
 * the HTTP thread (web_server.c) can call into this driver, and
 * spiAcquireBus() alone only keeps single 24-bit words intact -- two
 * interleaved frequency changes would mix their register sets. One
 * mutex for both synths, since they share the bus anyway. */
static MUTEX_DECL(lmx_mtx);

/* ====================================================================
 * Baseline register template
 *
 * Full 113-register set, address-descending (R112 first, R0 last --
 * already the order the datasheet's power-up sequence, §7.5.1,
 * requires: "program registers ... in REVERSE order from highest to
 * lowest"). Lifted verbatim from julia/test_fixtures/lmx2594_example.txt,
 * a real, previously-validated dump for this project (its per-line
 * R<n>-vs-encoded-address cross-check already passed, per
 * IMPLEMENTATION.md §10.2) -- used here as a known-good starting
 * point for every register synth_set_frequency() does NOT need to
 * touch, rather than hand-transcribing ~70 scattered field defaults
 * from the datasheet.
 *
 * IMPORTANT CAVEAT: this dump was captured for some specific
 * frequency plan whose fOSCin/fPD was never independently confirmed
 * (see LMX_FPD_HZ above) -- decoding its own PLL_N/CHDIV/VCO_SEL
 * fields is consistent with fPD = 200 MHz landing fVCO exactly at
 * the VCO4/VCO5 boundary (12000 MHz), which is suggestive but not
 * proof. Treat every register below as "known to produce a locked,
 * working part" for whatever design it came from, NOT as verified
 * for your actual board's reference oscillator -- re-capture a fresh
 * baseline (e.g. via PLLatinumSim) against your real OSCin/doubler/
 * PLL_R config if this assumption turns out wrong on real hardware.
 *
 * Only registers R0-R78 ("General", Table 22) plus this project's
 * captured R79-R112 are included; per Table 22, R79-R106 only matter
 * if ramping (RAMP_EN) is used (baseline has it off) and R107-R112
 * are readback-only, so writing them is harmless but not required --
 * kept here anyway since they're part of the one validated dump.
 * ==================================================================== */
#define LMX_NUM_BASELINE_REGS 113

static const uint32_t lmx_baseline_regs[LMX_NUM_BASELINE_REGS] = {
    0x700000UL, 0x6F0000UL, 0x6E0000UL, 0x6D0000UL, 0x6C0000UL, 0x6B0000UL,
    0x6A0000UL, 0x690021UL, 0x680000UL, 0x670000UL, 0x660000UL, 0x650011UL,
    0x640000UL, 0x630000UL, 0x620000UL, 0x610888UL, 0x600000UL, 0x5F0000UL,
    0x5E0000UL, 0x5D0000UL, 0x5C0000UL, 0x5B0000UL, 0x5A0000UL, 0x590000UL,
    0x580000UL, 0x570000UL, 0x560000UL, 0x550000UL, 0x540000UL, 0x530000UL,
    0x520000UL, 0x510000UL, 0x500000UL, 0x4F0000UL, 0x4E0003UL, 0x4D0000UL,
    0x4C000CUL, 0x4B0AC0UL, 0x4A0000UL, 0x49003FUL, 0x480001UL, 0x470081UL,
    0x46C350UL, 0x450000UL, 0x4403E8UL, 0x430000UL, 0x4201F4UL, 0x410000UL,
    0x401388UL, 0x3F0000UL, 0x3E0322UL, 0x3D00A8UL, 0x3C0000UL, 0x3B0001UL,
    0x3A8001UL, 0x390020UL, 0x380000UL, 0x370000UL, 0x360000UL, 0x350000UL,
    0x340820UL, 0x330080UL, 0x320000UL, 0x314180UL, 0x300300UL, 0x2F0300UL,
    0x2E07FCUL, 0x2DC0DFUL, 0x2C1FE3UL, 0x2B0000UL, 0x2A0000UL, 0x290000UL,
    0x280000UL, 0x2703E8UL, 0x260000UL, 0x250304UL, 0x240030UL, 0x230004UL,
    0x220000UL, 0x211E21UL, 0x200393UL, 0x1F43ECUL, 0x1E318CUL, 0x1D318CUL,
    0x1C0488UL, 0x1B0002UL, 0x1A0DB0UL, 0x190624UL, 0x18071AUL, 0x17007CUL,
    0x160001UL, 0x150401UL, 0x14E048UL, 0x1327B7UL, 0x120064UL, 0x11012CUL,
    0x100080UL, 0x0F064FUL, 0x0E1E70UL, 0x0D4000UL, 0x0C5001UL, 0x0B0018UL,
    0x0A10D8UL, 0x091604UL, 0x082000UL, 0x0740B2UL, 0x06C802UL, 0x0500C8UL,
    0x040A43UL, 0x030642UL, 0x020500UL, 0x010808UL, 0x00251CUL,
};

/* Datasheet Table 8 (Channel Divider Segments) -- CHDIV[4:0] register
 * code, equivalent division value, and the OutMin/OutMax bounds TI
 * already computed from the 7.5-15 GHz VCO range divided by that
 * value. Division 72 additionally requires fVCO <= 11.5 GHz, which
 * its own OutMin/OutMax bounds (104.167-159.722 MHz) already respect
 * given the 7.5-15 GHz VCO range, so no separate check is needed. */
typedef struct {
    uint8_t chdiv_code;
    uint16_t division;
    float out_min_mhz;
    float out_max_mhz;
} lmx_chdiv_entry_t;

static const lmx_chdiv_entry_t lmx_chdiv_table[] = {
    {  0,   2, 3750.0f,    7500.0f    },
    {  1,   4, 1875.0f,    3750.0f    },
    {  2,   6, 1250.0f,    2500.0f    },
    {  3,   8,  937.5f,    1437.5f    },
    {  4,  12,  625.0f,     958.333f  },
    {  5,  16,  468.75f,    718.75f   },
    {  6,  24,  312.5f,     479.167f  },
    {  7,  32,  234.375f,   359.375f  },
    {  8,  48,  156.25f,    239.583f  },
    {  9,  64,  117.1875f,  179.6875f },
    { 10,  72,  104.167f,   159.722f  },
    { 11,  96,   78.125f,   119.792f  },
    { 12, 128,   58.594f,    89.844f  },
    { 13, 192,   39.0625f,   59.896f  },
    { 14, 256,   29.297f,    44.922f  },
    { 15, 384,   19.531f,    29.948f  },
    { 16, 512,   14.648f,    22.461f  },
    { 17, 768,    9.766f,    14.974f  },
};
#define LMX_NUM_CHDIV_ENTRIES (sizeof(lmx_chdiv_table) / sizeof(lmx_chdiv_table[0]))

/* ---- small helpers: find/patch a register's 16-bit data field by
 * its 7-bit address within a working copy of the register set. ---- */
static uint16_t lmx_reg_get(const uint32_t *regs, int n, uint8_t addr)
{
    for (int i = 0; i < n; i++) {
        if (((regs[i] >> 16) & 0x7Fu) == addr) {
            return (uint16_t)(regs[i] & 0xFFFFu);
        }
    }
    return 0;
}

static void lmx_reg_set(uint32_t *regs, int n, uint8_t addr, uint16_t data)
{
    for (int i = 0; i < n; i++) {
        if (((regs[i] >> 16) & 0x7Fu) == addr) {
            regs[i] = ((uint32_t)addr << 16) | data;
            return;
        }
    }
}

/* ---- SPI transaction: one 24-bit word, MSB first, CS framed manually
 * (halconf.h sets SPI_SELECT_MODE_NONE -- see the comment there -- so
 * SPIConfig has no ssline/ssport/sspad field to fill in here). ---- */
static const SPIConfig lmx_spicfg = {
    .circular = false,
    .slave    = false,
    .data_cb  = NULL,
    .error_cb = NULL,
    /* Conservative baud rate divider (well under the 50 MHz write
     * limit) -- fine to raise once real hardware is characterized. */
    .cr1      = SPI_CR1_BR_2 | SPI_CR1_BR_0,
    .cr2      = 0,
};

static void lmx_spi_write_word(int synth, uint32_t word24)
{
    uint8_t tx[3] = {
        (uint8_t)(word24 >> 16),
        (uint8_t)(word24 >> 8),
        (uint8_t)(word24 >> 0),
    };

    spiAcquireBus(LMX_SPID);
    palClearLine(lmx_cs_lines[synth]);
    spiSend(LMX_SPID, 3, tx);
    palSetLine(lmx_cs_lines[synth]);
    spiReleaseBus(LMX_SPID);
}

/* ==================================================================
 * synth.h contract
 * ================================================================== */

void synth_init(void)
{
    palSetLineMode(LMX_SCK_LINE,  PAL_MODE_ALTERNATE(LMX_SPI_AF));
    palSetLineMode(LMX_MISO_LINE, PAL_MODE_ALTERNATE(LMX_SPI_AF));
    palSetLineMode(LMX_MOSI_LINE, PAL_MODE_ALTERNATE(LMX_SPI_AF));

    for (int s = 0; s < SYNTH_COUNT; s++) {
        /* idle high (deselected) before spiStart() */
        palSetLine(lmx_cs_lines[s]);
        palSetLineMode(lmx_cs_lines[s], PAL_MODE_OUTPUT_PUSHPULL);
#if LMX_MUXOUT_WIRED
        palSetLineMode(lmx_muxout_lines[s], PAL_MODE_INPUT);
#endif
    }

    spiStart(LMX_SPID, &lmx_spicfg);

    /* Recommended Initial Power-Up Sequence, datasheet §7.5.1:
     *   1-2. Reset, then remove reset.
     *   3-4. Program every general register, REVERSE order (already
     *        the order lmx_baseline_regs[] is stored in).
     *   5.   Wait 10 ms.
     *   6.   Program R0 one more time with FCAL_EN=1, so calibration
     *        runs from a stable state -- the baseline template's own
     *        R0 (0x00251C) already has FCAL_EN=1 (bit 3 set), so
     *        replaying it verbatim satisfies this step as-is. */
    for (int s = 0; s < SYNTH_COUNT; s++) {
        lmx_spi_write_word(s, 0x000002UL); /* R0: RESET=1 */
        lmx_spi_write_word(s, 0x000000UL); /* R0: RESET=0 */

        for (int i = 0; i < LMX_NUM_BASELINE_REGS; i++) {
            lmx_spi_write_word(s, lmx_baseline_regs[i]);
        }
    }

    chThdSleepMilliseconds(10);

    for (int s = 0; s < SYNTH_COUNT; s++) {
        lmx_spi_write_word(s, lmx_baseline_regs[LMX_NUM_BASELINE_REGS - 1]); /* R0 again */
    }
}

bool synth_set_frequency(int synth, uint64_t freq_hz)
{
    if (synth < 0 || synth >= SYNTH_COUNT) {
        return false;
    }

    float freq_mhz = (float)freq_hz / 1.0e6f;

    float fvco_mhz;
    bool use_chdiv;
    uint8_t chdiv_code = 0;

    if (freq_mhz >= 7500.0f && freq_mhz <= 15000.0f) {
        /* Direct VCO connect -- no channel divider needed. */
        use_chdiv = false;
        fvco_mhz = freq_mhz;
    } else {
        use_chdiv = false; /* set true below on a table match */
        fvco_mhz = 0.0f;
        for (size_t i = 0; i < LMX_NUM_CHDIV_ENTRIES; i++) {
            if (freq_mhz >= lmx_chdiv_table[i].out_min_mhz &&
                freq_mhz <= lmx_chdiv_table[i].out_max_mhz) {
                use_chdiv = true;
                chdiv_code = lmx_chdiv_table[i].chdiv_code;
                fvco_mhz = freq_mhz * (float)lmx_chdiv_table[i].division;
                break;
            }
        }
        if (!use_chdiv) {
            return false; /* outside the device's full tuning range */
        }
    }

    int core = lmx2594_vco_core(fvco_mhz);
    if (core < 0) {
        return false; /* fVCO landed outside 7.5-15 GHz -- shouldn't
                        * happen given the checks above, but be sure */
    }

    int cpg = lmx2594_select_cpg(fvco_mhz);
    if (cpg < 0) {
        return false;
    }
    uint8_t cpg_code = (uint8_t)cpg;

    /* N-divider: N + NUM/DEN = fVCO / fPD. PLL_DEN fixed at 1e6 for
     * ~fPD/1e6 Hz resolution (200 Hz at LMX_FPD_HZ=200MHz) -- ample
     * for this application; raise it if finer steps are ever needed
     * (up to the 32-bit field's real limit). */
    float n_total = (fvco_mhz * 1.0e6f) / LMX_FPD_HZ;
    uint32_t pll_n = (uint32_t)n_total;
    float frac = n_total - (float)pll_n;

    const uint32_t pll_den = 1000000UL;
    uint32_t pll_num = (uint32_t)(frac * (float)pll_den + 0.5f);
    if (pll_num >= pll_den) {
        pll_num = 0;
        pll_n += 1;
    }

    if (pll_n > 0x7FFFFUL) {
        return false; /* PLL_N is a 19-bit field (R34[2:0]:R36[15:0]) */
    }

    /* Table 2 (Minimum N-Divider Restrictions), for MASH_ORDER=3 --
     * the order baked into the baseline template's R44 and never
     * touched here. If R44/MASH_ORDER is ever changed, this table
     * (and PFD_DLY_SEL below) needs to change with it. */
    uint32_t min_n;
    uint8_t pfd_dly_sel;
    if (fvco_mhz <= 10000.0f) {
        min_n = 36;
        pfd_dly_sel = 3;
    } else {
        min_n = 40;
        pfd_dly_sel = 4;
    }
    if (pll_n < min_n) {
        return false; /* below the modulator's supported N for this fVCO */
    }

    uint8_t vco_sel = (uint8_t)(core + 1); /* VCO_SEL: 1=VCO1 .. 7=VCO7 */
    uint8_t out_mux = use_chdiv ? 0 : 1;   /* OUTx_MUX: 0=chdiv, 1=VCO */

    uint32_t regs[LMX_NUM_BASELINE_REGS];
    memcpy(regs, lmx_baseline_regs, sizeof(regs));

    uint16_t d;

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 75);
    d = (uint16_t)((d & ~(0x1Fu << 6)) | ((uint16_t)chdiv_code << 6));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 75, d);

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 46);
    d = (uint16_t)((d & ~0x3u) | (out_mux & 0x3u));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 46, d);

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 45);
    d = (uint16_t)((d & ~(0x3u << 11)) | ((uint16_t)out_mux << 11));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 45, d);

    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 43, (uint16_t)(pll_num & 0xFFFFu));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 42, (uint16_t)(pll_num >> 16));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 39, (uint16_t)(pll_den & 0xFFFFu));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 38, (uint16_t)(pll_den >> 16));

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 37);
    d = (uint16_t)((d & ~(0x3Fu << 8)) | ((uint16_t)pfd_dly_sel << 8));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 37, d);

    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 36, (uint16_t)(pll_n & 0xFFFFu));

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 34);
    d = (uint16_t)((d & ~0x7u) | (uint16_t)((pll_n >> 16) & 0x7u));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 34, d);

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 20);
    d = (uint16_t)((d & ~(0x7u << 11)) | ((uint16_t)vco_sel << 11));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 20, d);

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 14);
    d = (uint16_t)((d & ~(0x7u << 4)) | ((uint16_t)cpg_code << 4));
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 14, d);

    d = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, 0);
    d = (uint16_t)(d | (1u << 3)); /* FCAL_EN = 1, triggers VCO calibration */
    lmx_reg_set(regs, LMX_NUM_BASELINE_REGS, 0, d);

    /* Datasheet §7.5.2 "Recommended Sequence for Changing Frequencies":
     * N-divider, then NUM/DEN, then FCAL_EN=1 -- generalized here to
     * every field this retune may have touched, address-descending,
     * with R0/FCAL_EN written last as required. */
    static const uint8_t write_order[] = { 75, 46, 45, 43, 42, 39, 38, 37, 36, 34, 20, 14, 0 };
    chMtxLock(&lmx_mtx);
    for (size_t i = 0; i < sizeof(write_order); i++) {
        uint8_t addr = write_order[i];
        uint16_t data = lmx_reg_get(regs, LMX_NUM_BASELINE_REGS, addr);
        lmx_spi_write_word(synth, ((uint32_t)addr << 16) | data);
    }
    chMtxUnlock(&lmx_mtx);

    return true;
}

bool synth_is_locked(int synth)
{
    if (synth < 0 || synth >= SYNTH_COUNT) {
        return false;
    }
#if LMX_MUXOUT_WIRED
    /* R0[2] MUXOUT_LD_SEL=1 (baked into the baseline template, bit 2
     * of R0's 0x251C is set) puts MUXOUT in plain digital lock-detect
     * mode -- no SPI readback timing needed (that path multiplexes
     * onto MUXOUT too, but with its own clocked timing; §7.5/Figure 2
     * -- deliberately not used here since it needs the same pin). */
    return palReadLine(lmx_muxout_lines[synth]) != PAL_LOW;
#else
    return false; /* MUXOUT not wired yet -- CONFIGURE lmx_muxout_lines above */
#endif
}

bool synth_write_raw_register(int synth, uint32_t word24)
{
    if (synth < 0 || synth >= SYNTH_COUNT) {
        return false;
    }
    if (word24 & 0xFF000000UL) {
        return false; /* not a 24-bit value -- caller should already
                        * have range-checked this (synth.h contract) */
    }
    chMtxLock(&lmx_mtx);
    lmx_spi_write_word(synth, word24);
    chMtxUnlock(&lmx_mtx);
    return true;
}
