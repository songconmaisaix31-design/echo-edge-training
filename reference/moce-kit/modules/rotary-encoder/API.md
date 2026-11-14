# 旋转编码器 V1：驱动接入

公共头文件：[rotary_encoder.h](drivers/common/rotary_encoder.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus rotary_encoder_init(RotaryEncoder *, const RotaryEncoderConfig *, uint32_t);
BspStatus rotary_encoder_update(RotaryEncoder *, uint32_t);
BspStatus rotary_encoder_get(const RotaryEncoder *, uint32_t, RotaryEncoderReading *);
BspStatus rotary_encoder_set_position(RotaryEncoder *, int32_t);
BspStatus rotary_encoder_reset(RotaryEncoder *, uint32_t);
void rotary_encoder_deinit(RotaryEncoder *);
```

源文件依赖：`bsp_gpio.h`、`limits.h`、`rotary_encoder.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 编码器型号 | EC12E24204A8  |
| 编码方式 | 机械接触式增量编码，A/B两相输出  |
| 每圈脉冲数 | 24，不等同于倍频解码后的软件计数 脉冲/圈 |
| 每圈定位点数 | 24 个/圈 |
| 相位与引脚关系 | A→DO1，B→DO2，公共端C接地  |
| 按压输出支持 | 当前模块无按压输出；图中D/E未连接  |

数字 A/B 输入；VCC 范围待确认；未引出的按键不作为可用接口。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
