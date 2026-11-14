# 指纹模块  V1：驱动接入

公共头文件：[zw83080.h](drivers/common/zw83080.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus zw83080_init(Zw83080 *device,const Zw83080Config *config);
BspStatus zw83080_deinit(Zw83080 *device);
BspStatus zw83080_read_count(Zw83080 *device,uint16_t *count);
BspStatus zw83080_capture(Zw83080 *device);
BspStatus zw83080_generate(Zw83080 *device,uint8_t buffer);
BspStatus zw83080_search(Zw83080 *device,uint16_t *id,uint16_t *score);
BspStatus zw83080_enroll(Zw83080 *device,uint16_t id,uint32_t timeout_ms, Zw83080ProgressCallback callback,void *context);
BspStatus zw83080_delete_one(Zw83080 *device,uint16_t id);
```

源文件依赖：`bsp_uart.h`、`string.h`、`zw83080.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 指纹采集像素 | 80 × 64 像素 |
| 指纹存储容量 | 50 枚 |
| 默认 UART 波特率 | 57600 bit/s |
| 比对时间 | ＜0.8 s |

缺对应图纸和实物针号；待确认供电及模块 TX/RX 方向。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
