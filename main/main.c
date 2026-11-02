#include <stdio.h>
#include "i2s.h"

void app_main(void)
{
    init_i2s();
    i2s_loopback_task(NULL);
}
