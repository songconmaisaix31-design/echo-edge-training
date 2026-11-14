# 功放 V1：驱动接入

公共头文件：[max98357a.h](drivers/common/max98357a.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus max98357a_init(Max98357a *device, const Max98357aConfig *config);
BspStatus max98357a_start_pcm(Max98357a *device, const int16_t *pcm, size_t frames, unsigned channels, bool loop);
BspStatus max98357a_set_volume(Max98357a *device, unsigned volume_permille);
BspStatus max98357a_set_mute(Max98357a *device, bool muted);
BspStatus max98357a_query(Max98357a *device, Max98357aState *out);
BspStatus max98357a_stop(Max98357a *device, Max98357aState *out);
BspStatus max98357a_deinit(Max98357a *device);
```

源文件依赖：`bsp_critical.h`、`bsp_i2s.h`、`limits.h`、`max98357a.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 功放芯片型号 | MAX98357AETE+T  |
| I²S模式硬件增益 | 12；GAIN_SLOT直接接GND，采用固定硬件配置 dB |
| I²S声道与关断配置 | 5V供电时，SD_MODE经1MΩ上拉，配置为左右声道平均混合输出：(L＋R)/2；未引出独立关断控制接口  |

MAX98357A；5V 独立经 CN1 XT30 输入(pin1 GND/pin2 +5V)。GH5 pin1 未接，不是电源。扬声器桥接两端均不可接地。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 0/1 |
| ecw6700 | 1/1 |

F407 缺少公共 bsp_critical.h，当前不可直接构建此驱动。ECW 包已含兼容入口；音频模式、时钟、停止与声音仍需项目和实机验收。
