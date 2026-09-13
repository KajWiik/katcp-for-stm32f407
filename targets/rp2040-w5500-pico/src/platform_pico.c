#include "platform.h"
#include "pico/time.h"

uint64_t platform_now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}
