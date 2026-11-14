/* One declaration produces public inventory and vendor bindings.
 * Included with MOCE_BOARD_PORT defined by board.c; not a standalone C unit. */
MOCE_BOARD_PORT(BOARD_GPIO_0,
    BSP_CAP_BLOCKING, "AIN1",
    .instance = 0,
     .pins = pins_GPIO_0,
     .pin_count = 1,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_GPIO_1,
    BSP_CAP_BLOCKING, "AIN2",
    .instance = 1,
     .pins = pins_GPIO_1,
     .pin_count = 1,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_GPIO_2,
    BSP_CAP_BLOCKING, "BIN1",
    .instance = 2,
     .pins = pins_GPIO_2,
     .pin_count = 1,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_GPIO_3,
    BSP_CAP_BLOCKING, "BIN2",
    .instance = 3,
     .pins = pins_GPIO_3,
     .pin_count = 1,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_GPIO_4,
    BSP_CAP_BLOCKING, "CN13_SPI_CS",
    .instance = 4,
     .pins = pins_GPIO_4,
     .pin_count = 1,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_I2C_0,
    BSP_CAP_BLOCKING | BSP_CAP_RESTART | BSP_CAP_ASYNC, "CN9",
    .instance = 0,
     .pins = pins_I2C_0,
     .pin_count = 2,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_UART_0,
    BSP_CAP_BLOCKING | BSP_CAP_ASYNC | BSP_CAP_BACKGROUND_RX, "CN8",
    .instance = 0,
     .pins = pins_UART_0,
     .pin_count = 2,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_UART_1,
    BSP_CAP_BLOCKING | BSP_CAP_ASYNC | BSP_CAP_BACKGROUND_RX, "U18_UART1_ISP",
    .instance = 1,
     .pins = pins_UART_1,
     .pin_count = 2,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_PWM_0,
    BSP_CAP_BLOCKING, "CN7_SERVO",
    .instance = ECW6700_PWM_CCU6,
     .pins = pins_PWM_0,
     .pin_count = 2,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_PWM_1,
    BSP_CAP_BLOCKING, "PWMA",
    .instance = 1,
     .pins = pins_PWM_1,
     .pin_count = 1,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_PWM_2,
    BSP_CAP_BLOCKING, "PWMB",
    .instance = 2,
     .pins = pins_PWM_2,
     .pin_count = 1,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_SPI_0,
    BSP_CAP_BLOCKING | BSP_CAP_ASYNC | BSP_CAP_DISPLAY, "SPI_FIXED_PINS",
    .instance = 0,
     .pins = pins_SPI_0,
     .pin_count = 3,
     .output = false,
     .initial_high = false
)
MOCE_BOARD_PORT(BOARD_I2S_0,
    BSP_CAP_STREAM | BSP_CAP_DUPLEX_RX, "MIC_SPEAKER",
    .instance = 0,
     .pins = pins_I2S_0,
     .pin_count = 4,
     .output = false,
     .initial_high = false
)
/* IO19 / IO20 -> native F ADC channels 4 / 3. Runtime capability is
 * masked unless the checked ADC build is selected. Not yet measured on board. */
MOCE_BOARD_PORT(BOARD_ADC_0,
    BSP_CAP_BLOCKING, "CN2_ADC1_CN1_ADC2",
    .instance = 0,
    .pins = pins_ADC_0,
    .pin_count = 2,
    .output = false,
    .initial_high = false
)
