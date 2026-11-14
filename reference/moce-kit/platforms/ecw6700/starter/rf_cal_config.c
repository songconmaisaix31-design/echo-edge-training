#include "rf_cal_config.h"

int rf_cal_config_init(void)
{
    /* cal_info_obtain must stay at the end of this function. */
    cal_info_obtain();

    return 0;
}
