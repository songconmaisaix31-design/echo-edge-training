static const Ecw6700Timer timer_bindings[] = {{.id = BOARD_TIMER_0, .channel = 0},
                                              {.id = BOARD_TIMER_1, .channel = 1},
                                              {.id = BOARD_TIMER_2, .channel = 2},
                                              {.id = BOARD_TIMER_3, .channel = 3}};


static const BspTimerInfo timer_info[] = {{BOARD_TIMER_0, "PIT0_CH0_SHARED_PWM_IO16"},
                                          {BOARD_TIMER_1, "PIT0_CH1_SHARED_PWM_IO21"},
                                          {BOARD_TIMER_2, "PIT0_CH2_SHARED_PWM_IO22"},
                                          {BOARD_TIMER_3, "PIT0_CH3_SHARED_PWM_IO19"}};
static const BspTimerInventory timer_inventory = {.timers = timer_info, .count = 4};
const BspTimerInventory *bsp_board_get_timer_inventory(void)
{
    return &timer_inventory;
}

/* Installed radio inventory only, not implemented-operation or validation flags. */
static const BspRadioInfo radio_resources[] = {
    {BOARD_RADIO_0, "ECW6700", BSP_RADIO_WIFI | BSP_RADIO_BLE, 1}};
static const BspRadioInventory radio_inventory = {radio_resources, 1};
const BspRadioInventory *bsp_board_get_radio_inventory(void)
{
    return &radio_inventory;
}
