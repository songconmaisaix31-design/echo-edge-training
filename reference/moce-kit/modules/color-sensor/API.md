# 颜色识别 V1：驱动接入

公共头文件：[veml6040.h](drivers/common/veml6040.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus veml6040_init(Veml6040 *dev, const Veml6040Config *config);
BspStatus veml6040_configure(Veml6040 *dev, const Veml6040Settings *settings);
BspStatus veml6040_set_integration(Veml6040 *dev, Veml6040Integration integration);
BspStatus veml6040_set_mode(Veml6040 *dev, Veml6040Mode mode);
BspStatus veml6040_set_shutdown(Veml6040 *dev, bool shutdown);
BspStatus veml6040_sleep(Veml6040 *dev);
BspStatus veml6040_wake(Veml6040 *dev);
BspStatus veml6040_read_config(Veml6040 *dev, Veml6040ConfigReadback *out);
BspStatus veml6040_trigger(Veml6040 *dev);
BspStatus veml6040_wait_conversion(Veml6040 *dev);
BspStatus veml6040_read_channel(Veml6040 *dev, Veml6040Channel channel, uint16_t *out);
BspStatus veml6040_read_rgbw(Veml6040 *dev, Veml6040Sample *out);
BspStatus veml6040_measure(Veml6040 *dev, Veml6040Sample *out);
BspStatus veml6040_deinit(Veml6040 *dev);
BspStatus veml6040_integration_ms(Veml6040Integration integration, uint32_t *out);
BspStatus veml6040_conversion_wait_ms(Veml6040Integration integration, uint32_t *out);
BspStatus veml6040_green_sensitivity(Veml6040Integration integration, float *lux_per_count);
BspStatus veml6040_full_scale_lux(Veml6040Integration integration, float *out);
BspStatus veml6040_lux(uint16_t green, Veml6040Integration integration, float *out);
BspStatus veml6040_saturation_mask(const Veml6040Sample *sample, uint16_t threshold, uint8_t *mask);
BspStatus veml6040_cct_estimate(const Veml6040Sample *sample, float offset, float *kelvin);
```

源文件依赖：`bsp_i2c.h`、`math.h`、`veml6040.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 核心传感器型号 | VEML6040A3OG  |
| 采集通道 | R、G、B、W 四通道  |
| 单通道数据分辨率 | 16 bit |
| I²C 从机地址 | 0x10，7 位固定地址  |
| 支持的常用总线速率 | 100、400 kHz |
| 积分时间档位 | 40、80、160、320、640、1280 ms |

I2C；VCC 允许范围待确认。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
