# ESP32-S3 终端方案与任务

首个交付是终端通过 USB 独立运行，0.96 英寸 OLED 显示真实环境温度、相对湿度和环境气压，电脑串口显示同一批读数。RTX 5060 电脑处理心超影像。后续联动时，电脑通过串口发送待显示或播报的提示，ESP32-S3 执行显示和提示。

## 当前状态

已在 Windows 使用 `CH343 (COM4)` 连接终端。按 `115200` 波特监听 10 秒，收到 10 行循环输出 `50%R`、`50%G`、`50%B`。现有固件已能输出串口日志，待取得其工程目录和外设接线。通过模组丝印或启动日志确认 8 MB 是 Flash 还是 PSRAM。 [Espressif 模组配置](https://docs.espressif.com/projects/esp-idf/en/v5.2.1/esp32s3/hw-reference/esp32s3/user-guide-devkitc-1.html)

重新连接后，用 PowerShell 的 `[System.IO.Ports.SerialPort]::GetPortNames()` 确认端口，再用串口监视器按 `115200` 波特、8N1 复查固件输出。

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
