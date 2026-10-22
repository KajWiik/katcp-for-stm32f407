#ifndef PAC1954_H
#define PAC1954_H

#include <stdbool.h>
#include <stdint.h>

/* Microchip PAC1954 -- 4-channel I2C power/energy monitor. Same split
 * as synth.h: this header is the fixed contract sensor.c programs
 * against; the I2C transport lives in a target-specific driver
 * (targets/stm32-e407/src/pac1954_i2c.c), with a desktop stand-in in
 * test/pac1954_stub.c. The driver only hands back raw register codes
 * -- all code-to-volts/amps/watts conversion is done here in common/,
 * so it stays plain C and testable on the host (IMPLEMENTATION.md §6.4).
 *
 * Conversion assumes the chip's power-on default full-scale ranges:
 * unipolar VBUS (0..32 V) and unipolar VSENSE (0..100 mV), i.e.
 * NEG_PWR_FSR (register 0x1D) left at 0x0000. Nothing in this project
 * writes NEG_PWR_FSR; if bipolar/half-scale ranges are ever enabled,
 * the helpers below have to change with it. */

#define PAC1954_NUM_CHANNELS 4

#define PAC1954_VBUS_FSR_V    32.0   /* unipolar VBUS full scale */
#define PAC1954_VSENSE_FSR_V   0.1   /* unipolar VSENSE full scale */

/* CONFIGURE: shunt resistor on each channel, in ohms. Placeholder
 * values (10 mOhm -> 10 A full scale) until the real board's shunts
 * are known -- override per build with e.g.
 * UDEFS += -DPAC1954_RSENSE_CH1_OHMS=0.05 rather than editing here.
 * Getting this wrong scales current and power by a fixed, silent
 * factor; voltage is unaffected. */
#ifndef PAC1954_RSENSE_CH1_OHMS
#define PAC1954_RSENSE_CH1_OHMS 0.010
#endif
#ifndef PAC1954_RSENSE_CH2_OHMS
#define PAC1954_RSENSE_CH2_OHMS 0.010
#endif
#ifndef PAC1954_RSENSE_CH3_OHMS
#define PAC1954_RSENSE_CH3_OHMS 0.010
#endif
#ifndef PAC1954_RSENSE_CH4_OHMS
#define PAC1954_RSENSE_CH4_OHMS 0.010
#endif

/* One-time I2C setup, plus starting whatever background polling the
 * driver uses to keep a fresh snapshot (see pac1954_read_raw()). */
void pac1954_init(void);

/* Latest raw 16-bit VBUS and VSENSE register codes for channel ch
 * (0-based, 0..PAC1954_NUM_CHANNELS-1), both taken from the same
 * refresh. Must not block -- returns a cached snapshot. Returns false
 * if no recent valid snapshot exists (chip absent, I2C errors, or not
 * yet read since boot); the outputs are left untouched then. */
bool pac1954_read_raw(int ch, uint16_t *vbus, uint16_t *vsense);

/* ---- code -> physical units (unipolar default ranges, see above) ---- */

static inline double pac1954_vbus_volts(uint16_t code)
{
    return PAC1954_VBUS_FSR_V * (double)code / 65536.0;
}

static inline double pac1954_vsense_volts(uint16_t code)
{
    return PAC1954_VSENSE_FSR_V * (double)code / 65536.0;
}

#endif /* PAC1954_H */
