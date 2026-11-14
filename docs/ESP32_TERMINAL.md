# ESP32-S3 终端方案与任务

首个交付是终端通过 USB 独立运行，0.96 英寸 OLED 显示真实环境温度、相对湿度和环境气压，电脑串口显示同一批读数。RTX 5060 电脑处理心超影像。后续联动时，电脑通过串口发送待显示或播报的提示，ESP32-S3 执行显示和提示。

## 当前状态

**固件已接管，OLED 已点亮，ECW6700 桥已通。温湿度与语音仍未上线。**

接管前终端跑的是出厂固件，串口按 `115200` 波特循环输出 `50%R` / `50%G` / `50%B`（三路通道占空比），
只有日志、没有业务逻辑。现在已换成仓库内的自建固件，工程、构建命令、接线全部落地：

| 项 | 结论 | 详见 |
| --- | --- | --- |
| 主控 | ESP32-S3-DevKitC-1 **N16R8**（16 MB Flash / 8 MB Octal PSRAM），板载 CH343 → `COM4` | [hardware-facts](firmware/hardware-facts.md) |
| OLED | SSD1306 128×64，I²C **0x3C**，`SDA=GPIO41` / `SCL=GPIO42`，3.3 V 供电 | [firmware/oled-test](../firmware/oled-test/) |
| 当前固件 | `ecw-hud`：ECW 桥 + 状态屏二合一，屏上实时显示链路与总线状态 | [firmware/ecw-hud](../firmware/ecw-hud/) |
| ECW6700 | UART 115200 8N1，TX→`GPIO18`、RX→`GPIO17`，电池供电不接 3V3，命令须 `\r` 结尾 | [hardware-facts](firmware/hardware-facts.md) |
| 固件更新 | 走 WiFi OTA（`ota service_ota_start <url>`），安全启动未启用故不校验签名，无需进下载模式 | 同上 |
| 温湿度 | GXHT30C @ `0x44` **无应答**，总线本身健康 → 器件侧问题（官方注明 pin2/pin3 反接） | [pitfalls](firmware/pitfalls.md) |
| 语音 | SYN6288E 帧已能发到线上，但零回传，无法区分 TX 空焊 / 接线错 / 没供电 | [pitfalls](firmware/pitfalls.md) |

排查过程与全部踩坑记录见 [firmware/pitfalls.md](firmware/pitfalls.md)。

> 端口确认：PowerShell `[System.IO.Ports.SerialPort]::GetPortNames()`，或
> `python -c "import serial.tools.list_ports as p; print([x.device for x in p.comports()])"`。
> 认准 VID `1A86` PID `55D3`（CH343）；别和 ECW 调试板的 CH340（`1A86:7523`，`COM3`）搞混。

## 开发任务

| 顺序 | 实现 | 实机验收 |
| --- | --- | --- |
| 1. [接管固件](https://github.com/songconmaisaix31-design/echo-edge-training/issues/1) | 取得现有工程、构建命令和板卡接线。复用现有 GPIO 与串口初始化。 | 构建现有工程，核对模组、串口及已接外设。 |
| 2. [环境读数](https://github.com/songconmaisaix31-design/echo-edge-training/issues/2) | 确认 OLED 控制器、接口和引脚，以及温湿度、气压模块型号。用 USB 供电，逐个读传感器，把 °C、%RH 和 hPa 显示到 OLED。页面保留单色状态图标。 | OLED 和串口读数一致，与同处参考仪表比对。传感器拔除时清掉该项读数，其余读数继续刷新。 |
| 3. [心跳节拍](https://github.com/songconmaisaix31-design/echo-edge-training/issues/3) | 确认采集器件、接线和信号类型后，记录原始样本或节拍事件及时间戳。若为光学 PPG，信号稳定时显示估算脉率和节拍图标。 | 与同步参考记录对照，分别记录正常接触、移动及接触恢复时的输出。 |
| 4. [语音与振动](https://github.com/songconmaisaix31-design/echo-edge-training/issues/4) | 确认功放、扬声器和音频接口。用 16 kHz、16 位单声道 WAV 验证播放，再接 ESP-SR 离线中文文本转语音。为振动马达接入与类型和额定电压匹配的驱动，触发短提示。 | 播报一条输入文本，触发一次短振动。对比提示前后心跳采样是否丢失或受到干扰。 |
| 5. [双舵机与电源](https://github.com/songconmaisaix31-design/echo-edge-training/issues/5) | 核对舵机控制板接口、两只舵机的额定电压和电流。用匹配的独立稳压电源给舵机供电，逐只设置方向和机械行程。接电池前核对 12V 充电器铭牌输入、输出和电池化学体系及串数。 | 两只舵机分别空载到达指定位置。随后检查同时启动时供电电压和主控是否重启。 |
| 6. [电脑联动](https://github.com/songconmaisaix31-design/echo-edge-training/issues/6) | 从电脑发送一个实际提示，由终端更新 OLED 并触发对应播报或振动。采样和提示各自运行。 | 在设备上连续运行 30 分钟，记录提示到达、采样丢失和重启情况。 |

ECW6700 对应奕斯伟的 Wi-Fi SoC。语音联调按现有音频模块或 I²S 播放链接入，先根据器件丝印确认接法。 [奕斯伟芯片说明](https://www.eswincomputing.com/news/info/111.html) · [Espressif 中文语音合成](https://docs.espressif.com/projects/esp-sr/zh_CN/latest/esp32s3/speech_synthesis/readme.html)

下一次上板从第 1、2 项开始。需要现有固件工程或接线表、OLED 和两种环境传感器的型号。音频、心跳传感器、舵机和供电型号随各自任务核对。
