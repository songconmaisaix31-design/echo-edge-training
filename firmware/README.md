# 终端固件

ESP32-S3 侧的全部固件工程，以及配套的串口调试脚本。

板子是 **ESP32-S3-DevKitC-1 N16R8**（16 MB Flash / 8 MB PSRAM），走板载 **CH343** 转串口，
Windows 下枚举为 **COM4**（`1A86:55D3`）。不是原生 USB 口。

## 目录

| 目录 | 作用 | 状态 |
| --- | --- | --- |
| `edge-terminal/` | **终端主固件**：环境读数 + 脉率 + 中文播报 + 振动 + 双舵机 + 电脑提示协议（issue #2/#4/#5/#6） | 已上板；环境与 PPG 数据由 mock 供给 |
| `ecw-hud/` | ECW 桥 + SSD1306 状态屏二合一 | 已上板运行 |
| `oled-test/` | 纯点屏验证，带电气级引脚探测 | 已验证（一次命中 0x3C） |
| `ecw-bridge/` | ECW6700 桥连与联调（找引脚、找波特率、透传） | 已验证 |
| `sensor-hub/` | 三传感器直驱（温湿度 / 红外 / TTS） | 框架就绪，器件未通 |
| `probe/` | 早期探测固件（Blink / Probe / WireHelper / ScreenLink / EpdTest） | 保留作参考 |
| `tools/` | 电脑侧串口与烧录脚本 | 可用 |

## 编译与烧录

工具链是仓库自带的 `arduino-cli`（不依赖全局安装）：

```bash
./tools/bin/arduino-cli.exe --config-file ./tools/arduino-cli.yaml compile \
  --fqbn "esp32:esp32:esp32s3:FlashMode=dio,FlashSize=16M,CDCOnBoot=default,PSRAM=disabled,PartitionScheme=default" \
  --output-dir build_ecwhud EcwHud
```

`firmware/tools/flash.sh` 负责按分区烧录（不动 VFS 区），用法：

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
`tools/ppg_analyze.py` 把 `PPG LOG` 导出的原始采样独立复算成脉率，并检查时间戳单调性、
采样间隔与丢点（用于跟固件里的检测结果对照）。

## Mock 数据层

本批实物只有 OLED 和 TTS，环境传感器（GXHT30C / BMP388）、振动马达、舵机都没有。
为了让链路先跑通，`edge-terminal/` 内置了一层**数据源级 mock**：

- 位置在"器件本该给出的原始量"这一层，后面所有代码路径都是真的
- 器件一旦上线，mock 自动让位，绝不覆盖真实读数
- 每个 mock 值都带标记：屏上 `M` 角标、串口 `[MOCK:..]`、CSV 第 9 列
- `MOCK OFF` 即可回到全离线状态，用于验证"断开传感器该项显示不可用"
- 有算法的量（脉率、质量分）不直接造结论，由正式算法从 mock 波形里算出来

**带 mock 数据的日志不能当硬件验收证据。** 设计与命令见
[docs/firmware/mock-data.md](../docs/firmware/mock-data.md)。

## 与终端任务的对应

对应 [ESP32-S3 终端方案与任务](../docs/ESP32_TERMINAL.md)：

- 任务 1「接管固件」——已完成：工程、构建命令、板卡接线全部落地
- 任务 2「环境读数」——链路已通：OLED 实时刷新三项读数、单位、有效性角标；
  数据当前来自 mock，实物接线与对照测量仍待补
- 任务 3「心跳采集」——**物理上做不了**：套件里的 ITR9909 只有 LM393 比较输出（DO），
  没有模拟通道，采不到脉搏波。已把 PPG 采集链路和峰值检测算法做完（50 Hz，RR 中位数
  + 自适应不应期 + 相对幅度门），波形源是 mock；接 MAX30102 时只换波形源。
  详见 [docs/firmware/pulse.md](../docs/firmware/pulse.md)
- 任务 4~6——未开始（语音振动、舵机、电脑联动）

踩过的坑集中在 [docs/firmware/pitfalls.md](../docs/firmware/pitfalls.md)，
硬件关键事实见 [docs/firmware/hardware-facts.md](../docs/firmware/hardware-facts.md)。
