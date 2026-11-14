# 参考物料

## `moce-kit/` — MOCE 参赛套件（子集）

厂商下发的是 `MOCE-Contestant-Kit-1.0.0-alpha.1`，整包 16 MB / 778 个文件。
这里只收录对本项目有用的部分，**约 3 MB / 453 个文件**。

### 收录了什么

| 路径 | 内容 | 为什么收 |
| --- | --- | --- |
| `*.md` / `*.json` | 套件顶层说明：`README` `MODULES` `WIRING` `VALIDATION` `OPEN-ITEMS` `manifest.json` `driver-build-status.json` | 模块清单、接线约束、驱动编译状态的权威来源 |
| `modules/` | 37 个模块，每个含 `module.json` / `API.md` / `WIRING.md` / `SKILL.md` / `drivers/` | **最有价值的部分**。器件型号、I²C 地址、引脚定义、官方驱动全在这里 |
| `platforms/ecw6700/` | ECW6700 平台 BSP、`PORT-MAP.md`、`build_project.py`、`sdk-extension/` | 本项目用的 Wi-Fi SoC 就是它，OTA 与固件结构依赖这份 BSP |

### 没收录什么

`platforms/stm32f407/`（13 MB）—— 是 STM32F407 的 CMSIS 与 BSP 第三方源码，与本项目（ESP32-S3 终端）无关。
需要的话从原始套件取，校验值见套件的 `SHA256SUMS`。

### 怎么用

三个模块的官方定义直接决定了接线，**和实物丝印有出入**：

| 模块 | 器件 | 官方定义里的坑 |
| --- | --- | --- |
| `modules/temperature-humidity/` | GXHT30C，I²C **0x44** | `connection_status: blocked`，原文「图线文字与网标互换，须连续性测量实物 pin2/pin3」→ 按图纸接必然反接 |
| `modules/infrared/` | ITR9909 反射式 + LM393 | **不是 PIR**，需 1~5 cm 近距离反射 |
| `modules/text-to-speech/` | SYN6288E，UART **9600** 8N1 | 帧格式 `0xFD+lenH+lenL+data+XOR` |

`modules/oled-096/` 里的 `drivers/common/ssd1306.c` 是官方 SSD1306 实现，
可以对照 `firmware/ecw-hud/oled_ssd1306.h`（本项目自己写的精简版，无动态分配）互相印证。

### 其它相关模块

项目后续任务（心跳 / 语音振动 / 舵机）会用到：

- `modules/microphone/` — 语音输入
- `modules/speaker-amplifier/` — 功放
- `modules/vibration-motor/` — 振动马达
- `modules/mg90-servo-controller/`、`modules/mg90s-servo/` — 双舵机
- `modules/barometer/` — 气压（终端任务 2 需要 hPa 读数）
