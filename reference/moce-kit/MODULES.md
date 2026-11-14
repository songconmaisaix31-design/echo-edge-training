# 模块索引

原输入的 39 个独立模块：36 个外设/供电模块，加两个主控和一个 F407 转接板。主控和转接板收在 platforms/，避免重复 BSP。

| 模块 | 驱动组 | 接线状态 |
|---|---:|---|
| [空气质量传感器  V1](modules/air-quality/README.md) | 1 | schematic-derived; physical orientation unverified |
| [气压传感器 V1](modules/barometer/README.md) | 1 | schematic-derived; physical orientation unverified |
| [12V锂电池充电器 V1](modules/battery-charger/README.md) | 0 | blocked |
| [直流有刷电机驱动 V1](modules/brushed-dc-driver/README.md) | 1 | schematic-derived; physical orientation unverified |
| [颜色识别 V1](modules/color-sensor/README.md) | 1 | schematic-derived; physical orientation unverified |
| [4.2寸墨水屏 V1](modules/epaper-42/README.md) | 1 | schematic-derived; physical orientation unverified |
| [风扇模块V1](modules/fan/README.md) | 1 | blocked |
| [指纹模块  V1](modules/fingerprint/README.md) | 1 | blocked |
| [I2C转接板 V1](modules/i2c-adapter/README.md) | 0 | blocked |
| [九轴IMU模块V1](modules/imu-9axis/README.md) | 1 | schematic-derived; physical orientation unverified |
| [红外传感 V1](modules/infrared/README.md) | 1 | schematic-derived; physical orientation unverified |
| [摇杆 V1](modules/joystick/README.md) | 1 | schematic-derived; physical orientation unverified |
| [激光测距 V1](modules/laser-ranging/README.md) | 1 | schematic-derived; physical orientation unverified |
| [矩阵键盘 V1](modules/matrix-keypad/README.md) | 1 | schematic-derived; physical orientation unverified |
| [舵机控制 V1](modules/mg90-servo-controller/README.md) | 1 | schematic-derived; physical orientation unverified |
| [MG90S舵机V1](modules/mg90s-servo/README.md) | 0 | blocked |
| [麦克风 V1](modules/microphone/README.md) | 1 | schematic-derived; physical orientation unverified |
| [陀螺仪 V1](modules/mpu6050/README.md) | 1 | schematic-derived; physical orientation unverified |
| [0.96寸OLED V1](modules/oled-096/README.md) | 1 | schematic-derived; physical orientation unverified |
| [光敏电阻 V1](modules/photoresistor/README.md) | 1 | schematic-derived; physical orientation unverified |
| [电源模块 V1](modules/power-supply/README.md) | 0 | blocked |
| [功率开关 V1](modules/power-switch/README.md) | 0 | blocked |
| [旋转编码器 V1](modules/rotary-encoder/README.md) | 1 | schematic-derived; physical orientation unverified |
| [实时时钟 V1](modules/rtc/README.md) | 1 | schematic-derived; physical orientation unverified |
| [SD 卡V1](modules/sd-card/README.md) | 1 | schematic-derived; physical orientation unverified |
| [土壤湿度传感器 V1](modules/soil-moisture/README.md) | 1 | blocked |
| [功放 V1](modules/speaker-amplifier/README.md) | 1 | schematic-derived; physical orientation unverified |
| [温湿度传感器 V1](modules/temperature-humidity/README.md) | 1 | blocked |
| [文本转语音 V1](modules/text-to-speech/README.md) | 1 | schematic-derived; physical orientation unverified |
| [热敏电阻  V1](modules/thermistor/README.md) | 1 | schematic-derived; physical orientation unverified |
| [触摸按键 V1](modules/touch-button/README.md) | 1 | schematic-derived; physical orientation unverified |
| [触摸屏 V1](modules/touchscreen-2inch/README.md) | 2 | schematic-derived; physical orientation unverified |
| [TT双轴直流减速电机V1](modules/tt-motor/README.md) | 0 | blocked |
| [震动马达V1](modules/vibration-motor/README.md) | 1 | schematic-derived; physical orientation unverified |
| [水位传感器 V1](modules/water-level/README.md) | 1 | schematic-derived; physical orientation unverified |
| [灯板 V1](modules/ws2812-led-panel/README.md) | 1 | schematic-derived; physical orientation unverified |

[STM32F407 主控与转接板](platforms/stm32f407/README.md)；[ECW6700 主控](platforms/ecw6700/README.md)。
