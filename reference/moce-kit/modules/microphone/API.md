# 麦克风 V1：驱动接入

公共头文件：[msm261dgt003.h](drivers/common/msm261dgt003.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus msm261_init(Msm261 *device, const Msm261Config *config);
BspStatus msm261_start(Msm261 *device);
BspStatus msm261_query(Msm261 *device, BspAudioState *state);
BspStatus msm261_stop(Msm261 *device, BspAudioState *state);
BspStatus msm261_recorded(const Msm261 *device, const uint16_t **raw, size_t *halfwords);
void msm261_filter_init(Msm261Filter *filter);
BspStatus msm261_decode(Msm261Filter *filter, const uint16_t *raw, size_t n, int16_t *pcm, size_t capacity);
```

源文件依赖：`bsp_i2s.h`、`msm261dgt003.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| PDM声道选择方式 | 通过SW1将L/R引脚接至电源或GND；仅改变本麦克风的声道/数据时序配置，不增加采集通道数量  |

原理图为 MSM261DHT006、现有驱动为 MSM261DGT003。PDM 输出，pin2 未接；不得按五针 I2S 插头把 pin2 当 WS。型号及时钟模式未冻结。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
