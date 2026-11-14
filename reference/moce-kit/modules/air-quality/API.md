# 空气质量传感器  V1：驱动接入

公共头文件：[ags02ma.h](drivers/common/ags02ma.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
Ags02maStatus ags02ma_init(Ags02ma *device, const Ags02maConfig *config);
Ags02maStatus ags02ma_read_version(Ags02ma *device, uint8_t *version);
Ags02maStatus ags02ma_read_tvoc(Ags02ma *device, Ags02maSample *sample);
```

源文件依赖：`ags02ma.h`、`bsp_core.h`、`bsp_i2c.h`、`limits.h`、`stdbool.h`、`stdint.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 上电预热时间 | 至少120秒；长期存放后的通电老化时间另按手册要求执行 s |
| I²C通信速率上限 | 小于30，不使用100或400kHz默认配置 kHz |
| 默认I²C地址 | 7位地址0x1A；写地址字节0x34、读地址字节0x35；地址可修改并掉电保存  |
| 正常采样周期 | 不短于2秒，并遵守命令处理时间 s |
| TVOC测量范围 | 0～99999；超过9999属于拓展量程 ppb |
| TVOC测量精度条件 | 25℃、50%RH下典型30% F.S，最大误差50% F.S；典型精度适用0～9999ppb，拓  |

I2C；VCC 允许范围待确认，不据同插头推断可接 5V。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
