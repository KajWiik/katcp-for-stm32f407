#include "synth.h"

/* Stub so the project links and the KATCP server is fully testable over
 * the network before any synth-specific SPI code exists. Replace this
 * file with synth_lmx2594.c / synth_adf4351.c / etc. once the chip and
 * SPI wiring are settled -- the header contract above should not need
 * to change. */

void synth_init(void)
{
    /* TODO: spi_init(), gpio_init() for CS/LE and lock-detect once the
     * chip and pin assignment are decided. */
}

bool synth_set_frequency(uint64_t freq_hz)
{
    (void)freq_hz;
    return false; /* not implemented yet */
}

bool synth_is_locked(void)
{
    return false;
}
