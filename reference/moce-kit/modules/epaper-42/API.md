# 4.2寸墨水屏 V1：驱动接入

公共头文件：[epd0426.h](drivers/common/epd0426.h)、[epd0426_graphics.h](drivers/common/epd0426_graphics.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus epd0426_init(Epd0426 *device, const Epd0426Config *config);
BspStatus epd0426_show(Epd0426 *device, const uint8_t *frame, size_t length);
BspStatus epd0426_clear(Epd0426 *device, bool white);
BspStatus epd0426_sleep(Epd0426 *device);
BspStatus epd0426_canvas_init(Epd0426Canvas *canvas, uint8_t *buffer, size_t length, Epd0426Rotation rotation);
BspStatus epd0426_canvas_rotate(Epd0426Canvas *canvas, Epd0426Rotation rotation);
BspStatus epd0426_canvas_fill(Epd0426Canvas *canvas, bool white);
BspStatus epd0426_canvas_invert(Epd0426Canvas *canvas);
BspStatus epd0426_pixel(Epd0426Canvas *canvas, int16_t x, int16_t y, bool white);
BspStatus epd0426_pixel_read(const Epd0426Canvas *canvas, int16_t x, int16_t y, bool *white);
BspStatus epd0426_line(Epd0426Canvas *canvas, int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool white);
BspStatus epd0426_rect(Epd0426Canvas *canvas, int16_t x, int16_t y, uint16_t width, uint16_t height, bool filled, bool white);
BspStatus epd0426_circle(Epd0426Canvas *canvas, int16_t x, int16_t y, uint16_t radius, bool filled, bool white);
BspStatus epd0426_bitmap(Epd0426Canvas *canvas, int16_t x, int16_t y, uint16_t width, uint16_t height, const uint8_t *source, size_t length, bool transparent_white);
BspStatus epd0426_text(Epd0426Canvas *canvas, int16_t x, int16_t y, const char *text, size_t length, uint8_t scale, bool white, bool transparent_background);
```

源文件依赖：`bsp_core.h`、`bsp_gpio.h`、`bsp_spi.h`、`epd0426.h`、`epd0426_graphics.h`、`limits.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 配套面板型号 | GDEY0426T82；  |
| 显示控制器 | SSD1677  |
| 面板对角线尺寸 | 4.26 英寸 |
| 显示分辨率 | 800×480 像素 |
| 显示颜色 | 黑白  |
| BUSY有效电平 | 高电平表示忙  |
| 复位有效电平 | RES#低电平有效  |

SPI 写入；BUSY 为模块输出；分辨率/控制器和初始化参数以实际屏批次核对。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 2/2 |
| ecw6700 | 2/2 |
