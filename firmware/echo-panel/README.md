# EchoPanel — 床旁终端面板固件

一块 128×64 的 OLED，把后端最近一次切面推理的结果显示出来。不做推理、不做采集、
不碰 ECW 桥：这台设备就是一个显示器。

```
模型推理 ──> backend/panel.py ──GET /api/v1/panel/state──> ESP32-S3 ──I2C──> SSD1306
             (读最近一条 stanford_view 任务)                1 秒轮询
```

## 硬件

| 项 | 值 |
|---|---|
| 屏 | JMD0.96D-1 / SSD1306 128×64，I²C 地址 **0x3C** |
| SDA / SCL | **GPIO41 / GPIO42**（400 kHz，通不了会自动降到 100 kHz） |
| 供电 | 3V3 / GND |
| 串口 | CH343 → **COM4**（UART0，IO43/44，115200） |

避开 43/44（UART0 调试口）、17/18（ECW 桥）、26~37（Flash）、19/20（USB）。

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
| `POLL` | 立即拉一次并重绘 |
| `STATUS` | 状态汇总（屏/WiFi/后端/数据/轮询计数/堆） |
| `DUMP` | 导出 1KB 帧缓冲（十六进制，32 行 × 64 字符），用于核对屏上到底画了什么 |
| `DEMO <state>` | 不依赖网络演示五个界面态：`ready psax idle running offline` |
| `SCAN` | WiFi 扫描（排查"扫不到热点"用） |

## 屏幕版式

```
┌──────────────────────────────────────┐
│ ◭ ECHO EDGE              ▂▄▆ LIVE 6M │  logo + 产品名 / 信号格 + 状态与数据龄期
│   PEDIATRIC ECHO                     │  tagline 小字
├──────────────────────────────────────┤
│ PSAX 左室短轴                        │  切面代号（≤3 字母用 3 倍字号）+ 中文全称
│ ████████████████████████ 100%        │  置信度条 + 百分比
│ A4C 0.00   8F   FP32                 │  次概率 / 采样帧数 / 数值精度
└──────────────────────────────────────┘
```

五个状态：`ready`（有近 15 分钟内的结果）、`psax`/`a4c` 对应不同切面、`idle`
（还没跑过推理）、`running`、`offline`（连不上后端）。无效状态不画空进度条，
换成居中的中文状态页 —— 空进度条会被读成"置信度 0%"。

字号按字符数自适应：`view` 超过 3 个字符就降到 2 倍，否则 `PSAX` 会顶到中文上。
**中文字模只覆盖用到的字**（`panel_assets.h` 里的 `CJK_CHARSET`），改文案要重新导出。

## 后端这边的前提

1. uvicorn 必须绑 `0.0.0.0`（`work/local-services/run_api.py` 已经是）：
   `127.0.0.1` 对另一台设备不可达。
2. Windows 防火墙要放行 8036 的入站。热点网络被系统判为 **Private**，而
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

## 配套脚本

| 脚本 | 用途 |
|---|---|
| `tools/panel_design.py` | 用 PIL 画 128×64 1bpp 的五个界面态，导出 `panel_assets.h`（含中文字模） |
| `tools/dump_screen.py` | 拉取设备 `DUMP` 的帧缓冲还原成 PNG，用来核对版式（重叠/裁切） |
| `tools/run_view_once.py` | 绕过登录态直接跑一次真实推理，让面板从 idle 变 ready |
