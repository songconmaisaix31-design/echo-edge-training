# 激光测距 V1：驱动接入

公共头文件：[vl53l4cd.h](drivers/common/vl53l4cd.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
BspStatus vl53l4cd_init(Vl53l4cd *device, const Vl53l4cdConfig *config);
BspStatus vl53l4cd_start(Vl53l4cd *device);
BspStatus vl53l4cd_stop(Vl53l4cd *device);
BspStatus vl53l4cd_data_ready(Vl53l4cd *device, bool *ready);
BspStatus vl53l4cd_read(Vl53l4cd *device, Vl53l4cdReading *out);
BspStatus vl53l4cd_wait_read(Vl53l4cd *device, uint32_t wait_ms, Vl53l4cdReading *out);
```

源文件依赖：`bsp_i2c.h`、`vl53l4cd.h`、`vl53l4cd_defaults.inc`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 设计采用的传感器型号 | VL53L4CDV0DH/1  |
| 默认I²C地址 | 7位地址0x29；写地址字节0x52、读地址字节0x53  |
| 器件支持的I²C最高速率 | 最高1000；整板能否达到需满足总线时序和负载条件 kHz |
| 最小线性测距距离 | 1；不代表±1mm精度 mm |
| 器件最高测距频率 | 最高100；不能与33ms时间预算下的性能数据直接合并 Hz |
| 校准要求 | 所有应用需进行偏移校准；使用盖板时还需串扰校准  |
| 辅助接口装配状态 | 图中引出XSHUT、GPIO1、GND；连接器型号及实物装配待确认  |

图纸文件名写 VL53L0X，器件标注却为 VL53L4CDV0DH/1；驱动型号必须先与实物一致。额外 XSHUT/GPIO1 未纳入四针表。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
