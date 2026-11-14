# 热敏电阻  V1：驱动接入

公共头文件：[ntc_thermistor.h](drivers/common/ntc_thermistor.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus ntc_thermistor_init(NtcThermistor *device, const NtcThermistorConfig *config);
BspStatus ntc_thermistor_read(NtcThermistor *device, NtcThermistorReading *out);
BspStatus ntc_thermistor_reset_filter(NtcThermistor *device);
BspStatus ntc_thermistor_deinit(NtcThermistor *device);
BspStatus ntc_thermistor_convert(const NtcThermistorConfig *config, double input, NtcThermistorReading *out);
BspStatus ntc_thermistor_resistance_to_c(const NtcThermistorConfig *config, double resistance_ohm, double *out_c);
BspStatus ntc_thermistor_c_to_resistance(const NtcThermistorConfig *config, double temperature_c, double *out_ohm);
```

源文件依赖：`bsp_adc.h`、`math.h`、`ntc_thermistor.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 热敏电阻型号 | MF52A103F3950(A1)  |
| 25℃零功率阻值 | 10，容差±1%；规格书测试功率≤0.1mW kΩ |
| B25/50值 | 3950，容差±1% K |
| 固定分压电阻 | 10kΩ； kΩ |
| 温度与输出趋势 | 度升高→NTC阻值下降→AO电压下降  |
| 25℃标称输出电压 | 约1.65；3.3V供电、两只分压电阻均取标称10kΩ、输出负载可忽略 V |
| NTC热时间常数 | 静止空气中约12； s |
| 温度换算依据 | 由分压计算阻值，再采用厂家阻温表插值或适用的模型换算；精度需校准验证  |
| 整板测温精度 | 尚未实测，不以NTC阻值容差代替整板温度误差  |

AO 接 ADC；温度换算需要与热敏型号、分压参数一致。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
