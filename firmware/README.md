# 终端固件

ESP32-S3 侧的全部固件工程，以及配套的串口调试脚本。

板子是 **ESP32-S3-DevKitC-1 N16R8**（16 MB Flash / 8 MB PSRAM），走板载 **CH343** 转串口，
Windows 下枚举为 **COM4**（`1A86:55D3`）。不是原生 USB 口。

## 目录

| 目录 | 作用 | 状态 |
| --- | --- | --- |
| `ecw-hud/` | **当前主力固件**：ECW 桥 + SSD1306 状态屏二合一 | 已上板运行 |
| `oled-test/` | 纯点屏验证，带电气级引脚探测 | 已验证（一次命中 0x3C） |
| `ecw-bridge/` | ECW6700 桥连与联调（找引脚、找波特率、透传） | 已验证 |
| `sensor-hub/` | 三传感器直驱（温湿度 / 红外 / TTS） | 框架就绪，器件未通 |
| `probe/` | 早期探测固件（Blink / Probe / WireHelper / ScreenLink / EpdTest） | 保留作参考 |
| `tools/` | 电脑侧串口与烧录脚本 | 可用 |

## 编译与烧录

使用 Arduino CLI 1.6.0 和 Arduino-ESP32 3.3.12。从仓库根目录执行以下 PowerShell 命令。`arduino-cli` 可以是 PATH 中的命令，也可以替换成安装位置的完整路径。

```powershell
$index = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
arduino-cli core update-index --additional-urls $index
arduino-cli core install esp32:esp32@3.3.12 --additional-urls $index

$sketch = '.\work\firmware\EcwHud'
$output = '.\work\firmware\build_ecwhud'
New-Item -ItemType Directory -Force $sketch | Out-Null
Copy-Item .\firmware\ecw-hud\* $sketch -Force
$board = 'esp32:esp32:esp32s3:FlashMode=dio,FlashSize=16M,CDCOnBoot=default,PSRAM=disabled,PartitionScheme=default'
arduino-cli compile --fqbn $board --output-dir $output $sketch
```

Arduino CLI 要求主 `.ino` 与草图目录同名，因此在 Git 忽略的 `work/` 下使用 `EcwHud` 目录。`firmware/tools/flash.sh` 使用以脚本目录为基准的构建产物和 `boot_app0.bin` 路径。使用前按脚本核对文件位置。

2026 年 11 月 15 日用上述板型完成 EcwHud 编译，生成应用、bootloader、分区表、合并映像、ELF 和 map 文件。应用 `EcwHud.ino.bin` 为 330,000 字节，编译报告的程序存储和全局变量占用分别为 329,849 字节和 24,880 字节。通过 `COM4` 将提交 `ab960ff` 对应的 bootloader、分区表、`boot_app0.bin` 和应用分区写入 ESP32-S3，esptool 5.6.0 校验四段写入数据均通过。连接阶段识别到 16 MB Flash 和 8 MB PSRAM。

从仓库根目录运行串口工具：

```powershell
python -X utf8 .\firmware\tools\serial_cli.py COM4 --baud 115200 --script 'HELP|3;BUSC|14'
```

`HELP` 返回当前固件的命令。`BUSC` 扫描 `GPIO41`／`GPIO42` 上的 I²C 设备，输出实际发现的地址。

烧录后，`HELP` 返回 `EcwHud / 桥 + 状态屏`，`BUSC` 找到一个设备 `0x3C`。`HUD TEXT LINK OK`、`HUD OFF` 和 `HUD ON` 均返回成功回执。以电脑的单调时钟测量从发送 `HUD TEXT` 到收到对应完整回执的时间，连续发送 10 条不同文本均成功，中位数 42.931 ms，范围 42.080–43.939 ms。测试最后使用 `HUD ON` 恢复状态页。OLED 实际画面、断开屏幕后的失败回执、语音或振动、连续采样和 30 分钟联动仍需现场验收。

按分区烧录时使用以下命令：

```bash
bash flash.sh EcwHud COM4      # 桥 + 状态屏
bash flash.sh OledTest COM4    # 只点屏
bash flash.sh EcwBridge COM4   # 只桥
```

烧录参数固定：`--flash_mode dio --flash_freq 80m --flash_size 16MB`，
分区表 `default`，`boot_app0.bin` 烧在 `0xe000`。

## 接线

### OLED（SSD1306 128×64，I²C 0x3C）

| 模块 | ESP32-S3 |
| --- | --- |
| `GND` | `GND` |
| `VCC` | `3V3`（**只能 3.3V**） |
| `SCL` | `GPIO42` |
| `SDA` | `GPIO41` |

方向固定：`SDA=41 / SCL=42` 能扫到，对调后全零命中 —— 接反是全黑，不是花屏。

### ECW6700

| ECW | ESP32-S3 |
| --- | --- |
| TX | `GPIO18` |
| RX | `GPIO17` |
| GND | `GND`（必须共地） |

ECW 由自带电池供电，**不接 3V3**。console 波特率 115200，命令必须以 `\r` 结尾。

### 三传感器（待通）

| 器件 | 接口 | 引脚 | 状态 |
| --- | --- | --- | --- |
| GXHT30C 温湿度 | I²C 0x44 | 与屏共总线（41/42） | 未上线 |
| ITR9909 红外 | 数字 DO | `GPIO1` | 未验证 |
| SYN6288E TTS | UART 9600 8N1 | 待定 | 零回传 |

## 引脚禁区

| 引脚 | 原因 |
| --- | --- |
| `GPIO43` / `GPIO44` | 板载 UART0（TXD0/RXD0，CH343 调试口）。切成输入会串口失声，烧录报 `TX path seems to be down` |
| `GPIO26`~`GPIO37` | Flash / 八线 PSRAM |
| `GPIO19` / `GPIO20` | USB D- / D+ |
| `GPIO0` | BOOT（板载 10k 上拉，可作探测对照点） |

`GPIO41` / `GPIO42` 是 JTAG 脚（MTDI/MTMS），但走 CH343 烧录不冲突，也不是 strapping 脚，可自由使用。

## 串口协议

`ecw-hud` 与 `ecw-bridge` 都是 **115200 8N1 + 行命令**（`\r` 或 `\n` 结尾）：

```
PROBE                       引脚电平探测，判断线有没有真的接上
SCAN [毫秒]                  全引脚 × 全波特率监听
HUNT <baud> [秒]            单波特率轮询所有脚（期间复位对端）
LISTEN <pin> <baud> [秒]    盯一个脚看原始字节
SYNC <tx> <rx> [秒]         发 0xA5 试对端下载模式
BRIDGE <tx> <rx> <baud>     透传 COM4 <-> ECW，ESC 退出
SEND <tx> <rx> <baud> <hex> 发一串十六进制再听 1 秒
BUSC                        I²C 总线扫描（结果同时上屏）
HUD ON|OFF|TEXT <str>       状态屏开关 / 全屏大字
OLEDRST                     重新初始化屏
```

`tools/ecw_bridge_cli.py` 是电脑侧封装，免去手敲 `BRIDGE` 前缀。

## 与终端任务的对应

对应 [ESP32-S3 终端方案与任务](../docs/ESP32_TERMINAL.md)：

- 任务 1「接管固件」——已完成：工程、构建命令、板卡接线全部落地
- 任务 2「环境读数」——进行中：OLED 已通并能显示状态，温湿度 0x44 未上线
- 任务 3~6——未开始（心跳、语音振动、舵机、电脑联动）

踩过的坑集中在 [docs/firmware/pitfalls.md](../docs/firmware/pitfalls.md)，
硬件关键事实见 [docs/firmware/hardware-facts.md](../docs/firmware/hardware-facts.md)。
