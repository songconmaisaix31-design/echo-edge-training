# 0.96寸OLED V1：驱动接入

公共头文件：[ssd1306.h](drivers/common/ssd1306.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus ssd1306_init(Ssd1306 *display, const Ssd1306Config *config);
BspStatus ssd1306_clear(Ssd1306 *display, bool on);
BspStatus ssd1306_pixel(Ssd1306 *display, uint8_t x, uint8_t y, bool on);
BspStatus ssd1306_text(Ssd1306 *display, uint8_t x, uint8_t y, const char *text, size_t length);
BspStatus ssd1306_line(Ssd1306 *display, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, bool on);
BspStatus ssd1306_rect(Ssd1306 *display, uint16_t x, uint16_t y, uint16_t width, uint16_t height, bool filled, bool on);
BspStatus ssd1306_circle(Ssd1306 *display, uint16_t cx, uint16_t cy, uint16_t radius, bool filled, bool on);
BspStatus ssd1306_bitmap(Ssd1306 *display, uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint8_t *data, size_t data_size, size_t stride, bool foreground, bool transparent);
BspStatus ssd1306_text_utf8(Ssd1306 *display, uint16_t x, uint16_t y, const char *text, size_t byte_length, const Ssd1306Font *font, bool foreground, bool transparent);
BspStatus ssd1306_flush(Ssd1306 *display);
BspStatus ssd1306_invert(Ssd1306 *display, bool invert);
BspStatus ssd1306_power_off(Ssd1306 *display);
```

源文件依赖：`bsp_i2c.h`、`ssd1306.h`、`ssd1306_font.inc`、`ssd1306_graphics.inc`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|

I2C，图注 0x78 为线上写地址，对应公共 API 的 7bit 地址 0x3C。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
