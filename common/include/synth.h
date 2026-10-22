#ifndef SYNTH_H
#define SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/* Chip-specific implementation lives in synth_<part>.c (not yet written --
 * depends on which IC you settle on). This header is the fixed contract
 * the KATCP command layer programs against, so katcp_commands.c doesn't
 * need to change once the chip is picked. */

/* Number of synthesizers on the board. They share one SPI bus, each
 * with its own chip select. Every function below takes a 0-based
 * `synth` index (0..SYNTH_COUNT-1) and returns false for an index out
 * of range; the KATCP and web layers number them 1..SYNTH_COUNT for
 * users and convert. */
#define SYNTH_COUNT 2

/* One-time SPI + GPIO (every CS/LE, muxout/lock-detect if used) setup,
 * and power-up programming of every synth. */
void synth_init(void);

/* Program synth's output to freq_hz. Returns true on success (valid range,
 * SPI write completed), false otherwise. */
bool synth_set_frequency(int synth, uint64_t freq_hz);

/* Returns true if synth's lock-detect line reports locked.
 * (Stubbed false-always until wired to real hardware.) */
bool synth_is_locked(int synth);

/* Writes one raw 24-bit register word verbatim over SPI -- the exact
 * on-the-wire format LMX2594-style parts use: bits [23:16] are the
 * register address, bits [15:0] are the data, R/W bit implicit as a
 * write. word24 must fit in 24 bits (caller/command layer validates
 * range; this function assumes it's already checked). This is
 * deliberately the *only* interpretation applied -- no frequency
 * math, no charge-pump-current calculation, no VCO-curve awareness.
 * It exists so a precomputed register dump (e.g. from PLLatinumSim)
 * can be replayed byte-for-byte, foolproof but not usable for a
 * frequency sweep -- see IMPLEMENTATION.md for the fuller discussion
 * of why that's a deliberate, separate problem from ?synth-frequency. */
bool synth_write_raw_register(int synth, uint32_t word24);

#endif /* SYNTH_H */
