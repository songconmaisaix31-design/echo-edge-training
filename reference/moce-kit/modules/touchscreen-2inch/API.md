# 触摸屏 V1：驱动接入

公共头文件：[st7789.h](drivers/part-1/st7789.h)、[cst816d.h](drivers/part-2/cst816d.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus st7789_init(St7789 *display, const St7789Config *config);
BspStatus st7789_command(St7789 *display, uint8_t command, const uint8_t *data, size_t length);
BspStatus st7789_fill_rect(St7789 *display, uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t rgb565);
BspStatus st7789_finish(St7789 *display);
BspStatus st7789_write_pixels(St7789 *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels, size_t pixel_count);
BspStatus st7789_set_rotation(St7789 *d, unsigned rotation);
BspStatus st7789_sleep(St7789 *d);
BspStatus st7789_wake(St7789 *d);
BspStatus st7789_recover(St7789 *d);
BspStatus cst816d_init(Cst816d *touch, const Cst816dConfig *config);
BspStatus cst816d_read(Cst816d *touch, BspTouchEvent *event);
BspStatus cst816d_set_rotation(Cst816d *t, unsigned rotation);
BspStatus cst816d_sleep(Cst816d *t);
BspStatus cst816d_wake(Cst816d *t);
BspStatus cst816d_recover(Cst816d *t);
```

源文件依赖：`bsp_display.h`、`bsp_gpio.h`、`bsp_i2c.h`、`bsp_touch.h`、`cst816d.h`、`st7789.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 模块型号 | ZJY200K-IF05DCT  |
| 屏幕 | 2.0 英寸 TFT LCD，带电容触摸（G+F）  |
| 分辨率 | 240 × 320 RGB  |
| 显示颜色 | 262K  |
| 显示模式 | Normally Black  |
| 接口 | LCD：4 线 SPI 或 8-bit 8080 并口；触摸：I²C  |
| LCD 驱动 IC | ST7789P3  |
| 触摸驱动 IC | CST816D  |
| 背光 | 3 颗白色 LED 并联；约 3.0V，典型 60mA、最大 90mA  |

屏 SPI 与触摸 I2C 分别初始化；BLK/复位/IRQ 均需按 Board 的独立资源配置。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 2/2 |
| ecw6700 | 2/2 |
