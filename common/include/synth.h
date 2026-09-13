#ifndef SYNTH_H
#define SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/* Chip-specific implementation lives in synth_<part>.c (not yet written --
 * depends on which IC you settle on). This header is the fixed contract
 * the KATCP command layer programs against, so katcp_commands.c doesn't
 * need to change once the chip is picked. */

/* One-time SPI + GPIO (CS/LE, muxout/lock-detect if used) setup. */
void synth_init(void);

/* Program the output to freq_hz. Returns true on success (valid range,
 * SPI write completed), false otherwise. */
bool synth_set_frequency(uint64_t freq_hz);

/* Returns true if the synth's lock-detect line reports locked.
 * (Stubbed false-always until wired to real hardware.) */
bool synth_is_locked(void);

#endif /* SYNTH_H */
