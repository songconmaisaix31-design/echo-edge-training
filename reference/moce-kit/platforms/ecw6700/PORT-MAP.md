# Board 到芯片引脚的索引

从随包 Board 源码摘录。含条件编译资源，不代表当前所选组合全部可用；能力、限制和启用宏以原源码为准。引脚顺序为绑定数组顺序，使用前对照模块的信号方向和平台 API 注释，不据数组次序猜连接器针号。

| Board 标识 | 标签 | 绑定引脚数组 |
|---|---|---|
| `BOARD_GPIO_0` | AIN1 | 5 |
| `BOARD_GPIO_1` | AIN2 | 4 |
| `BOARD_GPIO_2` | BIN1 | 0 |
| `BOARD_GPIO_3` | BIN2 | 1 |
| `BOARD_GPIO_4` | CN13_SPI_CS | 11 |
| `BOARD_I2C_0` | CN9 | 24, 23 |
| `BOARD_UART_0` | CN8 | 17, 2 |
| `BOARD_UART_1` | U18_UART1_ISP | 9, 10 |
| `BOARD_PWM_0` | CN7_SERVO | 15, 16 |
| `BOARD_PWM_1` | PWMA | 22 |
| `BOARD_PWM_2` | PWMB | 21 |
| `BOARD_SPI_0` | SPI_FIXED_PINS | 14, 12, 13 |
| `BOARD_I2S_0` | MIC_SPEAKER | 6, 7, 8, 3 |
| `BOARD_ADC_0` | CN2_ADC1_CN1_ADC2 | 19, 20 |

主控数字 ID/别名见 board/mainboard/ports.h；转接板见 board/sheild/ 对应 ports.h。GPIO/ADC 资源需要 config 显式选择，Board 不是运行默认配置。

协议绑定顺序：I2C0 IO24=SCL/IO23=SDA；UART0 IO17=TX/IO2=RX；UART1 IO9=TX/IO10=RX（ISP/console共享）；SPI0 IO14=SCK/IO12=MISO/IO13=MOSI，片选另由GPIO4 IO11提供。ADC0逻辑源0=IO19(CN2)、源1=IO20(CN1)，不是SDK通道编号。定时器PIT资源与PWM共享，列出资源不等于已预留。
