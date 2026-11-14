# 文本转语音 V1：驱动接入

公共头文件：[syn6288e.h](drivers/common/syn6288e.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus syn6288e_init(Syn6288e *device, const Syn6288eConfig *config);
BspStatus syn6288e_speak_gbk(Syn6288e *device, const uint8_t *text, size_t length);
BspStatus syn6288e_command(Syn6288e *device, Syn6288eCommand command);
BspStatus syn6288e_read_reply(Syn6288e *device, uint8_t *reply);
BspStatus syn6288e_deinit(Syn6288e *device);
```

源文件依赖：`bsp_uart.h`、`limits.h`、`string.h`、`syn6288e.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 核心语音合成芯片 | SYN6288E  |
| UART通信格式 | 8位数据位、无校验、1位停止位，无硬件流控；默认9600bps，支持19200及38400bps  |
| 语音合成文本编码 | GB2312、GBK、BIG5、Unicode；发送数据须与命令中的编码选择一致。  |
| 单次语音合成文本最大长度 | 200；按编码后的字节数计算，超长文本由主控分段。 byte |
| 串口命令帧格式 | 帧头0xFD＋2字节数据区长度＋数据区；长度高字节在前；按对应命令定义填写参数及异或校验；最大帧长度206字节。  |
| UART接收反相实现 | 板上RX通过Q1 SS8050及外围电阻进行反相后接入芯片RXD；主控从U1发送时不再重复反相。  |
| 音量调节能力 | 16级；文本前景音量与背景音乐音量可分别控制，具体控制标记按手册执行  |
| 词语语速调节能力 | 支持6级词语语速调整，通过文本控制标记设置。  |
| 串口状态回传定义 | 0x4A初始化成功；0x41命令接收成功；0x45命令接收失败；0x4E正在播音；0x4F空闲。  |

RX 为模块输入、TX 为模块输出，分别接主控 TX、RX。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
