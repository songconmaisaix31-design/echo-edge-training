# 固件与实机验收记录

这个目录记录 ESP32-S3 终端在**实机**上的结论：硬件事实、踩过的坑、每个 issue 的验收状态。
原则只有一条：**没实测过的内容标为未完成，不用 mock 数据冒充真机数据。**

## 五个硬件 issue 的状态

| Issue | 主题 | 状态 | 记录 |
|---|---|---|---|
| #2 | OLED + 串口显示环境读数 | 🟡 链路通，数据来自 mock | [mock-data.md](mock-data.md) · [terminal-wiring.md](terminal-wiring.md) |
| #3 | 采集心跳并显示节拍 | 🟡 算法与链路完成，器件物理上采不到 | [pulse.md](pulse.md) · [samples/](samples/) |
| #4 | 中文播报与短振动 | 🟡 播报帧验证；WAV 做不到；无马达 | [voice.md](voice.md) |
| #5 | 双舵机与电源 | ⛔ 缺件（无舵机/无电源） | [servo.md](servo.md) |
| #6 | 电脑提示 + 30 分钟联动 | ✅ 实机长跑 | [link-test.md](link-test.md) |

状态含义：

- ✅ 用实物跑完，指标达标
- 🟡 代码与链路已验证，但关键输入是 mock 或缺一件实物
- ⛔ 缺件，做不了

## 通用参考

| 文件 | 内容 |
|---|---|
| [hardware-facts.md](hardware-facts.md) | 引脚禁区、I²C 地址、ECW 寄存器/OTA/安全启动等实测事实 |
| [pitfalls.md](pitfalls.md) | 踩坑记录，每条带「现象 → 根因 → 判据」 |
| [mock-data.md](mock-data.md) | 数据源级 mock 的设计与三条硬规则 |

## 一句话结论

**#6 已用实物验证链路**（屏幕在线、TTS 发帧、串口工作）。其余四项等待器件：
温湿度/气压模块没接、PPG 器件不存在（只有 DO 型红外）、无马达、无舵机。
把这些写清楚的目的是：下一个拿到器件的人知道从哪继续，而不是从头再排查一遍。
