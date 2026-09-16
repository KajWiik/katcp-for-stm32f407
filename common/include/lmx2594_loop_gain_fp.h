/*
 * lmx2594_loop_gain_fp.h
 *
 * Floating-point charge-pump (CPG) selection for the TI LMX2594, to
 * keep PLL loop gain (and therefore loop bandwidth/damping) roughly
 * constant across its 7 VCO cores, for a FIXED physical loop filter.
 *
 * Uses `float` throughout (not `double`) since the STM32F407 is a
 * Cortex-M4F with a single-precision-only hardware FPU; double would
 * silently fall back to slow software emulation.
 *
 * Assumes the phase-detector frequency (fPD) is constant for this
 * design, i.e. N = fVCO / fPD is directly proportional to fVCO. If
 * that's not true for your frequency plan, pass N explicitly instead
 * of deriving everything from fVCO alone -- see the comment inside
 * lmx2594_icp_ideal_ma().
 *
 * Reference: LMX2594 datasheet SNAS696C -- Table 6 (VCO Core Ranges),
 * Table 7 (VCO Gain) and its Equation 4, and Section 7.6.3 (CPG
 * register, R14[6:4]).
 */

#ifndef LMX2594_LOOP_GAIN_FP_H
#define LMX2594_LOOP_GAIN_FP_H

#include <stdint.h>
#include <math.h>

/* ---- CONFIGURE THESE TWO ----
 * The VCO frequency your physical loop filter (R2/C1/C2/C3/R3/C4) was
 * actually designed/simulated for, and the charge-pump current used
 * in that design. Everything else is normalized relative to this
 * point. */
#define LMX2594_FVCO_REF_MHZ   12000.0f   /* e.g. 12 GHz */
#define LMX2594_ICP_REF_MA         9.0f   /* e.g. 9 mA -> CPG code 5 */

typedef struct {
    float fmin_mhz;
    float fmax_mhz;
    float kvco1;    /* MHz/V, at fmin_mhz */
    float kvco2;    /* MHz/V, at fmax_mhz */
} lmx2594_vco_band_t;

/* Datasheet Table 6 / Table 7, VCO1..VCO7 (the LMX2594 has 7 cores,
 * not 8 -- VCO_SEL, R20[13:11], only accepts 1-7). */
static const lmx2594_vco_band_t lmx2594_vco_bands[7] = {
    {  7500.0f,  8600.0f,  73.0f, 114.0f }, /* VCO1 */
    {  8600.0f,  9800.0f,  61.0f, 121.0f }, /* VCO2 */
    {  9800.0f, 10800.0f,  98.0f, 132.0f }, /* VCO3 */
    { 10800.0f, 12000.0f, 106.0f, 141.0f }, /* VCO4 */
    { 12000.0f, 12900.0f, 170.0f, 215.0f }, /* VCO5 */
    { 12900.0f, 13900.0f, 172.0f, 218.0f }, /* VCO6 */
    { 13900.0f, 15000.0f, 182.0f, 239.0f }, /* VCO7 */
};

/* Available charge-pump currents (mA) and their CPG register codes,
 * ascending by current -- R14[6:4] per Section 7.6.3. States 2 and 6
 * are reserved, so only these 6 currents actually exist. */
static const float   lmx2594_cpg_levels_ma[6] = { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f, 15.0f };
static const uint8_t lmx2594_cpg_codes[6]      = { 0,    4,    1,    5,    3,     7    };

/* Which of the 7 cores covers fvco_mhz. Returns 0..6, or -1 if out of
 * the device's 7.5-15 GHz VCO range. This is exact (not just a
 * calibration starting guess) if you force VCO_SEL yourself via
 * full-assist mode using this same core index. */
static inline int lmx2594_vco_core(float fvco_mhz)
{
    for (int i = 0; i < 7; i++) {
        if (fvco_mhz >= lmx2594_vco_bands[i].fmin_mhz &&
            fvco_mhz <= lmx2594_vco_bands[i].fmax_mhz) {
            return i;
        }
    }
    return -1;
}

/* Linear-interpolated Kvco (datasheet Equation 4), in MHz/V.
 * core must be a valid index from lmx2594_vco_core(). */
static inline float lmx2594_kvco(float fvco_mhz, int core)
{
    const lmx2594_vco_band_t *b = &lmx2594_vco_bands[core];
    return b->kvco1 + (b->kvco2 - b->kvco1) *
           (fvco_mhz - b->fmin_mhz) / (b->fmax_mhz - b->fmin_mhz);
}

/* Ideal (continuous) charge-pump current in mA to hold the loop's
 * natural frequency constant relative to the reference design point,
 * assuming fixed fPD so N is proportional to fVCO:
 *
 *   Icp_ideal = Icp_ref * (fVCO / fVCO_ref) * (Kvco_ref / Kvco)
 *
 * If your fPD is NOT fixed across the tuning range, replace the
 * fvco_mhz/FVCO_REF_MHZ ratio below with N/N_ref (actual feedback
 * divider ratio) instead -- the rest of the math is unchanged. */
static inline float lmx2594_icp_ideal_ma(float fvco_mhz, float kvco)
{
    int ref_core = lmx2594_vco_core(LMX2594_FVCO_REF_MHZ);
    float kvco_ref = lmx2594_kvco(LMX2594_FVCO_REF_MHZ, ref_core);

    return LMX2594_ICP_REF_MA * (fvco_mhz / LMX2594_FVCO_REF_MHZ) * (kvco_ref / kvco);
}

/* Nearest available CPG register code for a desired current. */
static inline uint8_t lmx2594_nearest_cpg_code(float icp_ideal_ma)
{
    int best = 0;
    float best_diff = fabsf(icp_ideal_ma - lmx2594_cpg_levels_ma[0]);
    for (int i = 1; i < 6; i++) {
        float diff = fabsf(icp_ideal_ma - lmx2594_cpg_levels_ma[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return lmx2594_cpg_codes[best];
}

/* Top-level entry point: given only the target VCO frequency (MHz,
 * i.e. before the channel divider), return the CPG register code to
 * write into R14[6:4] to keep loop bandwidth close to the reference
 * design point. Returns -1 if fvco_mhz is outside 7.5-15 GHz.
 *
 * Merging into your existing R14 word:
 *   r14 = (r14 & ~(0x7u << 4)) | ((uint16_t)cpg_code << 4);
 * Writing R14 does not trigger FCAL_EN, so this can be done on every
 * retune as a plain register write, no extra calibration needed. */
static inline int lmx2594_select_cpg(float fvco_mhz)
{
    int core = lmx2594_vco_core(fvco_mhz);
    if (core < 0) return -1;

    float kvco = lmx2594_kvco(fvco_mhz, core);
    float icp_ideal = lmx2594_icp_ideal_ma(fvco_mhz, kvco);

    return (int)lmx2594_nearest_cpg_code(icp_ideal);
}

#endif /* LMX2594_LOOP_GAIN_FP_H */
