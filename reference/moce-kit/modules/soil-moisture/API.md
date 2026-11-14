# 土壤湿度传感器 V1：驱动接入

公共头文件：[soil_moisture.h](drivers/common/soil_moisture.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus soil_moisture_init(SoilMoisture *device, const SoilMoistureConfig *config);
BspStatus soil_moisture_read(SoilMoisture *device, SoilMoistureSample *out);
BspStatus soil_moisture_calibrate(SoilMoisture *device, int64_t dry, int64_t wet);
BspStatus soil_moisture_clear_calibration(SoilMoisture *device);
BspStatus soil_moisture_query(const SoilMoisture *device, SoilMoistureState *out);
BspStatus soil_moisture_deinit(SoilMoisture *device);
```

源文件依赖：`bsp_adc.h`、`limits.h`、`soil_moisture.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 输出信号形式 | 经板载检波和平滑处理的单路模拟电压输出  |
| 检波网络标称 RC 时间常数 | 103.4 	ms |

未定位图纸；模拟/数字版本、供电、AO/DO 针号需确认，禁止套用光敏模块。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
