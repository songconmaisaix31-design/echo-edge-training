# 使用前需要冻结的资料

以下项目不是更换代码即可验证的事项。请由供货方/硬件负责人补齐；未确认前不要让代码生成器猜接线。

| 模块 | 需要确认 |
|---|---|
| [空气质量传感器  V1](modules/air-quality/WIRING.md) | I2C；VCC 允许范围待确认，不据同插头推断可接 5V。 |
| [气压传感器 V1](modules/barometer/WIRING.md) | I2C；VCC 允许范围待确认。 |
| [12V锂电池充电器 V1](modules/battery-charger/WIRING.md) | 无软件接口；电池串数、充电电压电流、插头极性须由供货方确认后使用。 |
| [颜色识别 V1](modules/color-sensor/WIRING.md) | I2C；VCC 允许范围待确认。 |
| [风扇模块V1](modules/fan/WIRING.md) | 当前两批图纸中未定位对应模块；禁止按通用三针线序猜供电和控制极性。 |
| [指纹模块  V1](modules/fingerprint/WIRING.md) | 缺对应图纸和实物针号；待确认供电及模块 TX/RX 方向。 |
| [I2C转接板 V1](modules/i2c-adapter/WIRING.md) | 无源转接；未定位对应图纸，需确认各路直通关系、供电轨及总线上拉并联值。 |
| [摇杆 V1](modules/joystick/WIRING.md) | 双 ADC 加按键输入；VCC 范围待确认，AO 不得超过主控 ADC 范围。 |
| [激光测距 V1](modules/laser-ranging/WIRING.md) | 图纸文件名写 VL53L0X，器件标注却为 VL53L4CDV0DH/1；驱动型号必须先与实物一致。额外 XSHUT/GPIO1 未纳入四针表。 |
| [MG90S舵机V1](modules/mg90s-servo/WIRING.md) | 舵机本体无自有驱动；接舵机控制板。具体运动版本、线色/针号、脉宽和机械限位待确认。 |
| [麦克风 V1](modules/microphone/WIRING.md) | 原理图为 MSM261DHT006、现有驱动为 MSM261DGT003。PDM 输出，pin2 未接；不得按五针 I2S 插头把 pin2 当 WS。型号及时钟模式未冻结。 |
| [电源模块 V1](modules/power-supply/WIRING.md) | 无软件接口；输入范围、各输出电压/电流及插头极性须按实物标签核对，不能用同线序代替电源规格。 |
| [功率开关 V1](modules/power-switch/WIRING.md) | 未定位对应模块图；负载额定值、供电及控制极性待确认。 |
| [旋转编码器 V1](modules/rotary-encoder/WIRING.md) | 数字 A/B 输入；VCC 范围待确认；未引出的按键不作为可用接口。 |
| [SD 卡V1](modules/sd-card/WIRING.md) | 3.3V SPI；随包驱动是裸扇区层，不包含 FAT 文件系统。 |
| [土壤湿度传感器 V1](modules/soil-moisture/WIRING.md) | 未定位图纸；模拟/数字版本、供电、AO/DO 针号需确认，禁止套用光敏模块。 |
| [温湿度传感器 V1](modules/temperature-humidity/WIRING.md) | 禁止直接按表通电通信：图纸文字与网标互换，须连续性测量实物 pin2/pin3 并由供货方冻结线序。 |
| [热敏电阻  V1](modules/thermistor/WIRING.md) | AO 接 ADC；温度换算需要与热敏型号、分压参数一致。 |
| [TT双轴直流减速电机V1](modules/tt-motor/WIRING.md) | 电机本体经匹配驱动板供电；不能直接接 MCU IO。额定电压、堵转电流与编码器版本待确认。 |
| [水位传感器 V1](modules/water-level/WIRING.md) | 第二批图纸为数字输出版；旧模拟水位驱动/ADC 换算不能直接用于本版本，需新增适用数字驱动或应用读取 BSP GPIO。 |
| [灯板 V1](modules/ws2812-led-panel/WIRING.md) | 模块需 5V，三针不得误接 3.3V ADC；pin 数字方向待按实物输入端标记复核。3.3V 控制是否满足门限需确认。 |

所有线序按图纸针号给出；实物观察面、线束端是否翻转尚未逐件验收。问题反馈提供模块/硬件版本、主控/Board/SDK版本、接线表、返回码和最小日志；联系发放本包的赛事技术支持，具体联系人由主办方填写。

软件阻塞：F407 功放驱动依赖的公共 bsp_critical.h 不在当前F407 BSP中，本候选未修改BSP API或旧驱动；修复须经BSP维护者完成后重新交付。ECW 已有该兼容入口。

WS2812为CN3：1 NC/2 DI/3 GND；独立CN2 XT30供5V。激光驱动实际为VL53L4CD，与图纸器件标注一致，但图纸文件名为VL53L0X，仍需核对发放实物。SD裸扇区可用不等于有文件系统。
