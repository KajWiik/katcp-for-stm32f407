#include "pac1954.h"

/* Desktop-only stand-in for targets/stm32-e407/src/pac1954_i2c.c, same
 * role as synth_stub.c/platform_host.c: lets sensor.c's PAC1954
 * sensors (IMPLEMENTATION.md §6.4) link and exercise their
 * code-to-unit conversion in host tests without a chip attached.
 * Fixed, plausible raw codes -- not a model of the real part. Channel
 * 4 reports "no snapshot" to exercise the SENSOR_STATUS_ERROR path. */

void pac1954_init(void)
{
}

bool pac1954_read_raw(int ch, uint16_t *vbus, uint16_t *vsense)
{
    /* vbus 10240 -> 5.0000 V; vsense 32768 -> 50 mV -> 5 A at 10 mOhm */
    static const uint16_t fake_vbus[PAC1954_NUM_CHANNELS]   = { 10240, 6758, 24576, 0 };
    static const uint16_t fake_vsense[PAC1954_NUM_CHANNELS] = { 32768, 6554,  1311, 0 };

    if (ch < 0 || ch >= PAC1954_NUM_CHANNELS - 1) {
        return false;
    }
    *vbus   = fake_vbus[ch];
    *vsense = fake_vsense[ch];
    return true;
}
