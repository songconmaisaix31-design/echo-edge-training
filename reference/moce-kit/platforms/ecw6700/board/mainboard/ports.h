#ifndef MOCE_MAINBOARD_PORTS_H
#define MOCE_MAINBOARD_PORTS_H
#include "bsp_core.h"
#define BOARD_GPIO_0 BSP_PORT(BSP_GPIO, 0)
#define BOARD_GPIO_1 BSP_PORT(BSP_GPIO, 1)
#define BOARD_GPIO_2 BSP_PORT(BSP_GPIO, 2)
#define BOARD_GPIO_3 BSP_PORT(BSP_GPIO, 3)
#define BOARD_GPIO_4 BSP_PORT(BSP_GPIO, 4)
/* CN9: SCL IO24 / SDA IO23. ECW BSP 0.9.0 + B02P03 adaptation
 * i2c-rate-compat-1k-20261019 accepts integer target rate_hz 1000..400000.
 * Setup/TPM are quantized automatically; actual SCL remains unmeasured.
 * Select in project config; Board does not select/init speed. Existing
 * 100000/400000 encodings are retained. One active rate per controller;
 * finish requests and deinit/reinit before changing the target rate. */
#define BOARD_I2C_0 BSP_PORT(BSP_I2C, 0)
#define BOARD_UART_0 BSP_PORT(BSP_UART, 0)
#define BOARD_UART_1 BSP_PORT(BSP_UART, 1)
#define BOARD_PWM_0 BSP_PORT(BSP_PWM, 0)
#define BOARD_PWM_1 BSP_PORT(BSP_PWM, 1)
#define BOARD_PWM_2 BSP_PORT(BSP_PWM, 2)
#define BOARD_SPI_0 BSP_PORT(BSP_SPI, 0)
#define BOARD_I2S_0 BSP_PORT(BSP_I2S, 0)
#define BOARD_ADC_0 BSP_PORT(BSP_ADC, 0)
#define BOARD_ADC1_INDEX 0u /* CN2, IO19; not a native channel number */
#define BOARD_ADC2_INDEX 1u /* CN1, IO20; not a native channel number */
#endif

/* ADC signal -> input-only digital aliases, software binding 1.0.0-alpha.1. */
#define BOARD_ADC_0_DI_0 BSP_PORT(BSP_GPIO, 192)
#define BOARD_ADC_0_DI_1 BSP_PORT(BSP_GPIO, 193)
