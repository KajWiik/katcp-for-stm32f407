#include "hal.h"
#include "portab.h"

void portab_setup(void)
{
    /* Nothing board-specific needed beyond what board.c already does
     * at hal_lld_init() time. Kept as a named hook (matching ChibiOS's
     * usual demo structure) in case a future board revision needs
     * something here. */
}
