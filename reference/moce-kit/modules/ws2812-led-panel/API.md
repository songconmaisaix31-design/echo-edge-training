# 灯板 V1：驱动接入

公共头文件：[integration_requirements.h](drivers/common/integration_requirements.h)、[ws2812b_do_dma.h](drivers/common/ws2812b_do_dma.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus ws2812b_do_init(Ws2812bDo *device, const Ws2812bDoConfig *config);
BspStatus ws2812b_do_set_pixel(Ws2812bDo *device, unsigned x, unsigned y, Ws2812bDoColor color);
BspStatus ws2812b_do_fill(Ws2812bDo *device, Ws2812bDoColor color);
BspStatus ws2812b_do_draw_bitmap(Ws2812bDo *device, const uint8_t rows[8], Ws2812bDoColor foreground, Ws2812bDoColor background);
BspStatus ws2812b_do_set_brightness(Ws2812bDo *device, uint8_t brightness);
BspStatus ws2812b_do_start(Ws2812bDo *device);
BspStatus ws2812b_do_query(Ws2812bDo *device, Ws2812bDoState *out);
BspStatus ws2812b_do_stop(Ws2812bDo *device, Ws2812bDoState *out);
BspStatus ws2812b_do_deinit(Ws2812bDo *device);
```

源文件依赖：`bsp_timer_dma_gpio.h`、`string.h`、`ws2812b_do_dma.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 灯珠型号 | WS2812B-Mini-V6  |
| 灯珠数量 | 64 颗 |
| 单色灰度等级 | 每个 R、G、B 通道支持 256 级灰度 级 |
| 控制方式 | 单线串行级联控制  |
| 数据传输速率 | 800 kbit/s |
| 复位信号低电平时间 | 大于 280 μs |

CN3 pin1 未接、pin2 DI、pin3 GND。5V 经独立 CN2 XT30 输入(pin1 GND、pin2 +5V)，不要从三针信号口供电。3.3V 控制是否满足门限需确认。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
