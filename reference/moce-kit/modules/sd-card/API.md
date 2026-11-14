# SD 卡V1：驱动接入

公共头文件：[sd_card_spi_driver.h](drivers/common/sd_card_spi_driver.h)。结构体、错误码、单位和缓冲生命周期以头文件注释为准。

调用路径：应用业务 → 本模块驱动 → 主控 BSP 公共 API → 平台适配 → SDK → 外设。具体端口、速率、地址、GPIO 方向/上下拉、ADC 参考值和缓冲在项目 config 中选择。

先完成平台启动和 BSP 初始化，再初始化所用协议/IO，最后初始化模块。异步传输完成或明确归还缓冲后才能复用/释放；停止失败不能当资源已释放。驱动未提供的操作不得猜函数名。

入口索引（自动摘录，完整签名以原头文件为准）：

```c
SdCardState sd_card_get_state(const SdCard *card);
BspStatus sd_card_init(SdCard *card, const SdCardBinding *binding);
BspStatus sd_card_read_sector(SdCard *card, uint32_t lba, uint8_t data[SD_CARD_SECTOR_SIZE]);
BspStatus sd_card_write_sector(SdCard *card, uint32_t lba, const uint8_t data[SD_CARD_SECTOR_SIZE]);
BspStatus sd_card_read(SdCard *card, uint32_t lba, uint32_t count, uint8_t *data, uint32_t *completed);
BspStatus sd_card_write(SdCard *card, uint32_t lba, uint32_t count, const uint8_t *data, uint32_t *completed);
BspStatus sd_card_sync(SdCard *card);
BspStatus sd_card_deinit(SdCard *card);
```

源文件依赖：`bsp.h`、`sd_card_spi_driver.h`、`string.h`。C 库和 BSP 头由平台提供；需要 `math.h` 的功能在最终链接添加数学库。

| 参数 | 发布资料值 |
|---|---|
| 板级接口方式 | SPI方式；引出CS、SCLK、MISO、MOSI；未引出DAT1、DAT2  |
| 独立卡检测信号 | 卡座独立CD触点未引出至主控连接器，不能提供独立硬件插卡检测线  |

3.3V SPI；随包驱动是裸扇区层，不包含 FAT 文件系统。

验证：本交付不包含应用例程，也不把旧发布文字中的“已验证”继承为本包实机结论。逐模块目标编译与实机状态见根目录 VALIDATION.md。

本轮目标文件编译（不是模块最终链接或设备运行）：

| 主控 | 通过/文件数 |
|---|---|
| stm32f407 | 1/1 |
| ecw6700 | 1/1 |
