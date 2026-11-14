# 触摸按键 V1：驱动接入

公共头文件：[touch_key.h](drivers/common/touch_key.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus touch_key_init(TouchKey *self, const TouchKeyConfig *config);
BspStatus touch_key_read(TouchKey *self, TouchKeySample *out);
BspStatus touch_key_deinit(TouchKey *self);
```

源文件依赖：`bsp_gpio.h`、`touch_key.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 触摸检测芯片 | TTP223-TD，单键电容式触摸检测  |
| 触摸通道数 | 1 路 |
| 输出形式 | 芯片OUT为推挽数字输出，经R1 22Ω连接DO/SIG  |
| 输出模式配置 | STG、SLH由SW2配置，用于选择跟随或翻转模式，以及有效电平或上电初始状态；实际拨码位置需记录  |

模块输出接主控数字输入；确认触摸开关的有效电平和上电稳定时间。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
