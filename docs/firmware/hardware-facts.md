# 硬件关键事实

这张表里每一条都是本机实测或官方源码得出的，不是从数据手册抄的通用值。
排查时先看这里，能省掉大半猜测。

## 一、ESP32-S3 主控

| 项 | 值 |
| --- | --- |
| 型号 | ESP32-S3-DevKitC-1 **N16R8**（16 MB Flash / 8 MB Octal PSRAM） |
| 烧录口 | 板载 CH343 USB-UART，Windows 枚举 `COM4`（`1A86:55D3`） |
| 原生 USB 口 | `COM5`（`303A`），本项目不用 |
| 串口日志 | UART0，115200 8N1 |
| FQBN | `esp32:esp32:esp32s3:FlashMode=dio,FlashSize=16M,CDCOnBoot=default,PSRAM=disabled,PartitionScheme=default` |
| 工具链 | Arduino CLI 1.6.0 与 Arduino-ESP32 3.3.12，见[固件构建命令](../../firmware/README.md#编译与烧录) |

### 引脚禁区（踩过就知道疼）

| 引脚 | 原因 | 症状 |
| --- | --- | --- |
| `GPIO43` / `GPIO44` | 板载 UART0 的 TXD0 / RXD0，直连 CH343 | 切成输入后串口**彻底失声**；烧录报 `TX path seems to be down` |
| `GPIO26`~`GPIO37` | Octal Flash + PSRAM | 碰了直接不启动 |
| `GPIO19` / `GPIO20` | USB D- / D+ | 破坏 USB |
| `GPIO0` | BOOT 键，板载 10k 上拉 | 可作探测对照点（必须读 HIGH） |

`GPIO41` / `GPIO42` 是 JTAG（MTDI / MTMS），但走 CH343 烧录不冲突，也不是 strapping 脚 —— 可安全使用。

## 二、当前接线

| 外设 | 接法 | 状态 |
| --- | --- | --- |
| SSD1306 OLED | `GND`→GND，`VCC`→3V3，`SCL`→GPIO42，`SDA`→GPIO41 | 通（0x3C） |
| ECW6700 | TX→GPIO18，RX→GPIO17，GND 共地，**不接 3V3**（电池供电） | 通（115200） |
| GXHT30C 温湿度 | 与屏共 I²C 总线 | **不通**（0x44 无应答） |
| ITR9909 红外 | DO→GPIO1 | 未验证 |
| SYN6288E TTS | UART 9600 8N1 | **不通**（零回传） |

## 三、ECW6700（奕斯伟 Wi-Fi SoC）

| 项 | 值 |
| --- | --- |
| MAC（sta） | `E8:88:6C:55:C8:B6` |
| MAC（ap） | `EA:88:6C:55:C8:B6` |
| Console | UART，115200 8N1，**命令必须以 `\r` 结尾**（`\r\n` 无响应） |
| 固件形态 | 官方 `standalone` CLI，封装好的线程，**不能用杜邦线外接固件调试** |
| 分区 | 单 cpu 区 `0x14000`，size `0x3E0000`（3.9 MB），**无 A/B 槽** → OTA 类型 **AA (0xC2)** |
| 安全启动 | **未启用**：`ut_efuse read 0 4` = `0x00000000`，`ota_v3_auth.c:34` 直接 `return 0` → **OTA 不校验签名** |
| OTA 通道 | `ota service_ota_start <url>` → HTTP 下载 + 自动重启；无需进 DOWNLOAD_BOOT |
| WiFi | `wifi_sta <ssid> [password]` 可连；出厂 `ap+sta` 但未连接（ip 0.0.0.0） |

### UART0 寄存器（免烧录直接发字节）

| 项 | 值 |
| --- | --- |
| `MEM_BASE_UART0` | `0x20204000` |
| `MEM_BASE_UART1` | `0x20205000` |
| THR / RBR 偏移 | `+0x20` |
| LSR 偏移 | `+0x34`（bit0 RDR，bit5 THRE，bit6 TEMT） |

实测：`ut_uart open 0 9600 0 0` → ok；`read 0x20204034 4` → `00000060`；
`write 0x20204020 0x41` 后 LSR 仍 `60` → 字节确实上了线。

速度：8 ms/字节连续灌，10 字节的 SYN6288 帧 **85 ms** 发完。

## 四、三个传感器的官方定义

来自参赛套件 `reference/moce-kit/modules/`，与实物丝印有出入，以官方定义为准。

| 模块 | 器件 | 关键参数 | 陷阱 |
| --- | --- | --- | --- |
| 温湿度 | **GXHT30C** | I²C **0x44**，CN1 四针 | 官方 `connection_status: blocked`，原文「图线文字与网标互换，须连续性测量实物 pin2/pin3」→ **按图纸接必然 pin2/pin3 反接** |
| 红外 | **ITR9909 反射式 + LM393** | CN2 三针：1=3V3 / 2=DO / 3=GND | **不是 PIR**，需 1~5 cm 近距离反射才有输出 |
| TTS | **SYN6288E** | UART **9600** 8N1，帧 `0xFD+lenH+lenL+data+XOR` | 回传字节 0x4A / 0x41 / 0x45 / 0x4E / 0x4F |

## 五、I²C 总线现状

| 地址 | 器件 | 应答 |
| --- | --- | --- |
| `0x3C` | SSD1306 OLED | ✅ |
| `0x44` | GXHT30C 温湿度 | ❌ |

总线本身是健康的：`GPIO41`/`GPIO42` 在内部下拉模式下读 HIGH（外部有上拉），
无死锁，`BUSC` 全段扫描 10.5 s 完成。所以零命中是**器件侧**的问题，不是总线问题。

## 六、其它实测结论

- **`ut_gpio read` 对已被 pinmux 成外设的引脚一律返回 0** —— 连 IO9 正在当 UART1_TX 通信时也读 0。
  所以「读 0 说明没接线」这个推论在 ECW 上**不成立**。
- ECW 底板多数 IO 自带下拉（连未接的舵机口 IO15/16 也读 0），`ut_gpio pull` 判外部连接信噪比很低。
- ECW ADC 通道未标定：`ut_adc single 19 2800` 返回 `285440mV` 无效值，BSP 注明 "Not yet measured on board"
  → `GPIO19` 只能当数字输入用。
- SPI 上有烧录接口，接电脑后只多出 `COM3`（CH340），全波特率 0 字节；
  `eswin_tool` 没有 devices / 读 Flash / dry-run 子命令。
