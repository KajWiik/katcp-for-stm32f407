#include "synth.h"

/* Stub so the project links and the KATCP server is fully testable over
 * the network before any synth-specific SPI code exists.
 *
 * Relocated here from common/src/ now that a real driver exists
 * (targets/stm32-e407/src/synth_lmx2594.c) -- it now plays the same
 * role platform_host.c already does: a desktop-only stand-in, never
 * pulled into a real target's build. It stays useful for exercising
 * katcp_proto.c/katcp_commands.c host-side (IMPLEMENTATION.md §8)
 * without a chip attached, with ?synth-frequency/?synth-write-register
 * still cleanly reporting fail. Update the §8 build command to
 * reference test/synth_stub.c instead of common/src/synth_stub.c. */

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

bool synth_write_raw_register(uint32_t word24)
{
    (void)word24;
    return false; /* not implemented yet */
}
