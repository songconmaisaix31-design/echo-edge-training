# EchoPanel — 床旁终端面板固件

一块 128×64 的 OLED 加一个语音合成模块，把后端最近一次切面推理的结果"演"出来：
屏幕上有一只跟着心跳跳的小心脏，喇叭里把判断念一遍。

```
模型推理 ──> backend/panel.py ──GET /api/v1/panel/state──> ESP32-S3 ──I2C──> SSD1306
             (读最近一条 stanford_view 任务)                │
                                                             └──UART2 9600──> SYN6288E ──> 喇叭
```

不做推理、不做采集、不碰 ECW 桥：这台设备就是一个**会说话的显示器**。

## 硬件

| 项 | 值 |
|---|---|
| 屏 | JMD0.96D-1 / SSD1306 128×64，I²C 地址 **0x3C** |
| 屏接线 | SDA = **GPIO41**，SCL = **GPIO42**（400 kHz，不通会自动降到 100 kHz） |
| 语音 | SYN6288E，UART2 **9600 8N1**，TX = **GPIO5**，RX = **GPIO6** |
| 串口 | CH343 → **COM4**（UART0，IO43/44，115200） |

避开 43/44（UART0 调试口）、17/18（ECW 桥）、26~37（Flash）、19/20（USB）。
GPIO5 是 strapping 脚，模块 RX 悬空不影响启动，但如果模块把 5 脚拉住会进不了下载模式。

## 编译与烧写

工具链装在 WorkBuddy 的 tools 目录里，不在系统 PATH 上：

```bash
CLI=/c/Users/DW/WorkBuddy/2026-11-13-23-51-50/tools/bin/arduino-cli.exe
CFG=/c/Users/DW/WorkBuddy/2026-11-13-23-51-50/tools/arduino-cli.yaml
FQBN="esp32:esp32:esp32s3:FlashMode=dio,FlashSize=16M,CDCOnBoot=default,PSRAM=disabled,PartitionScheme=default"
OUT=D:/Codex/echo-edge-2026-11-15/work/echo-edge-settings/firmware/echo-panel/build/esp32.esp32.esp32s3

"$CLI" --config-file "$CFG" compile -b "$FQBN" --output-dir "$OUT" firmware/echo-panel
"$CLI" --config-file "$CFG" upload -p COM4 --fqbn "$FQBN" --input-dir "$OUT"
```

> `CDCOnBoot` 只有 `default`（=关闭）和 `cdc` 两个值。板子的日志走 CH343 也就是
> UART0，所以保持 `default`；填 `disabled` 会直接报 invalid value。

WiFi 凭据存在 NVS，**不写进源码**，仓库里不会带出热点密码。烧写不会擦 NVS，
所以配置一次之后重新烧固件仍然联网。

## 串口命令

| 命令 | 作用 |
|---|---|
| `WIFI "ssid" pass` | 写入热点并连接（存 NVS）。SSID 带空格/中文时用直引号包起来 |
| `WIFIOFF` | 擦除凭据 |
| `URL <url>` | 设置快照地址，默认 `http://172.20.10.2:8036/api/v1/panel/state` |
| `POLL` | 立即拉一次并重绘（同时退出演示态） |
| `STATUS` | 状态汇总（屏/WiFi/后端/数据/播报/轮询计数/堆） |
| `DUMP` | 导出 1KB 帧缓冲（十六进制，32 行 × 64 字符），用于核对屏上到底画了什么 |
| `DEMO <state>` | 不依赖网络演示界面态：`ready`(四腔心) `psax` `running` `offline` |
| `TTS <n>` | 播报第 n 条文本，并打印模块回执（排查接线用） |
| `SAY` | 按当前结果拼一句念一次 |
| `MUTE` | 播报开关 |
| `SCAN` | WiFi 扫描 |

## 屏幕版式

`screen.png` 是设备 `DUMP` 出来的真实帧缓冲还原图（不是设计稿）：

```
┌──────────────────────────────────────┐
│ ◭ ECHO EDGE              ▂▄▆ LIVE 3S │  logo + 产品名 / 信号格 + 状态与数据龄期
│   PEDIATRIC ECHO                     │  tagline 小字
├──────────┬───────────────────────────┤
│          │ PSAX                      │  切面代号（2 倍字）
│  (动画)  │ 左室短轴                  │  中文全称（16px 点阵）
│  42x42   │ ████████████ 100%         │  置信度条 + 百分比
└──────────┴───────────────────────────┘
```

加了动画之后右栏只剩 80px，原来那行"次概率 / 帧数 / 精度"撤掉了 —— 四行挤在
45px 里每行都贴边，而这几个数是排障用的，`STATUS` 里还有。

## 动画

五个状态五套精灵，全部是程序化图元（圆、矩形、像素点），不存位图：

| 状态 | 精灵 | 行为 |
|---|---|---|
| 认出四腔心 | 一整颗心，方孔眼睛 + 高光 + 微笑 + 腮红 | 按 `{1,1,0,0,-1,0,0,0}` 的档位搏动，第 3 拍眨眼 |
| 认出短轴 | 心肌圆环，腔里有两只眼 | 内径 8↔11 收缩，模拟心肌增厚 |
| 分析中 | 缩小的心，外圈三颗彗尾点 | 点绕 18px 的圆跑，8 帧一圈 |
| 待分析 | 睡着的心 | 闭眼，头顶飘 Z |
| 断线 | 裂开的心，眼睛变 X | 锯齿缝，静止 |

**屏幕上的形状就是模型看到的切面** —— 这是唯一的对外映射，写在 `spriteMode()` 里。
以后接上病理模型（EF、室壁运动之类），只改这一个函数就能让动画跟着病情变，
不用动渲染。

低置信度（< 0.65）时精灵会左右晃 1px 且嘴角朝下 —— 拿不准的时候不该笑。

全帧刷新是 1KB / 25ms，120ms 一帧只占 20% 的 I2C，所以动画不需要局部刷新。
真正拖慢它的是同步 HTTP：每轮轮询会把主循环卡住 100~400ms，所以有数据时把
轮询间隔放宽到 1.5 秒，让卡顿稀一点。

## 语音播报

SYN6288E 是**文本转语音**模块，不是音频解码器：输入只有 GB2312/GBK 文本帧，
它自己合成、自己 DAC、自己功放。所以不能播 WAV（那条 issue #4 的验收在这套
硬件上做不到），但"把模型判断念出来"正是它擅长的事。

帧格式：

```
0xFD | lenH | lenL | 0x01(合成播放) | 0x00(GB2312) | data... | XOR
```

`len = 2 + data 字节数`，XOR 覆盖从 0xFD 到 data 末尾。

文本在**打包阶段**就转成 GB2312（`tools/voice_lines.py` → `voice_lines.h`），
设备上不做 UTF-8→GB2312 转码 —— 那需要一整张码表。

句子是拼出来的，不是一句一条：

```
"本次影像判断为" + "左室短轴切面" + "，置信度" + "很高"
```

所以要加一个切面只加一个词条，不用把组合数乘出来。播报时机：

- 首次联网：`回声边缘床旁终端已就绪`
- 新结果（`seq` 变了）一次：整句判断。**只认 seq**，否则 1 秒一次轮询会变复读机
- 状态跳变：过期 / 分析中 / 没有结果 / 断线各一次
- `MUTE` 可静音

## 后端这边的前提

1. uvicorn 必须绑 `0.0.0.0`（`work/local-services/run_api.py` 已经是）：
   `127.0.0.1` 对另一台设备不可达。
2. Windows 防火墙要放行 8036 的入站。热点网络被判为 **Private**，而
   `C:\Python313\python.exe` 原本只在 **Public** 下放行过，结果是 SYN 被静默丢弃
   —— 现象是设备端 `code=-1`、PC 上一个连接都看不到。加一条规则（需要管理员）：

   ```powershell
   netsh advfirewall firewall add rule name=EchoEdgePanel8036 dir=in action=allow ^
       protocol=TCP localport=8036 profile=private,public
   ```

   判断是不是这个原因：在 PC 上另起一个监听端口，让设备去连。如果连新端口也
   一个包都收不到，就是防火墙按程序拦了，跟后端代码无关。

## 踩过的坑

- **读完 body 之后白等 1.5 秒**。服务端默认 keep-alive，body 收完后
  `available()==0` 但 `connected()` 仍为真，原来的"读到连接关闭或 1500ms 兜底"
  每轮都空转满 1.5 秒，实测单次请求 1.7s。改成按 `Content-Length` 读满即停，
  现在 < 400ms。
- **`setConnectTimeout(600)` 太紧**。DHCP 之后首包要等 ARP 落地，实测会摸到
  600ms 附近，于是开机第一次请求稳定失败（`code=-1`）。放到 1000ms。
- **打开 COM4 会复位板子**（CH343 的 DTR/RTS 接在 EN 上）。所以每次
  `python -c "serial.Serial('COM4')"` 都会看到一遍启动日志，WiFi 要 2~3 秒才回来，
  这期间的 `STATUS` 显示"未连接"是正常的。
- **`%` 不在 5×7 字库里**，`snprintf("%d%%")` 会画出一个空白字符 —— 看起来像是
  数字少了一位。字模已补在 `OLED_CHARSET` 末尾。
- **白底白点等于没画**。分析中的旋转点一开始落在白色心形上，屏上什么都看不到；
  轨道半径必须大于心形的外接半径。
- **模块不回执不代表没收到**。只接 TX 的接法（很多模块只需要收）不会有回执，
  所以 `TTS` 命令的"1.5 秒内没有回执"只能说明串口没回音，声音还在不在要听。

## 配套脚本

| 脚本 | 用途 |
|---|---|
| `tools/panel_design.py` | 用 PIL 画 128×64 1bpp 的界面态，导出 `panel_assets.h`（含中文字模） |
| `tools/voice_lines.py` | 把播报文案转成 GB2312 字节，导出 `voice_lines.h` |
| `tools/dump_screen.py` | 拉取设备 `DUMP` 的帧缓冲还原成 PNG，用来核对版式（重叠/裁切） |
| `tools/run_view_once.py` | 绕过登录态直接跑一次真实推理，让面板从 idle 变 ready |
