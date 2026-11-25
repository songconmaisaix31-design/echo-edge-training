# 工作站、切面模型与终端联调

部署使用 `main` 上的工作站代码和 EdgeTerminal 固件。前端和后端分别来自 [PR #13](https://github.com/songconmaisaix31-design/echo-edge-training/pull/13) 和 [PR #14](https://github.com/songconmaisaix31-design/echo-edge-training/pull/14)，由 [联调 PR #20](https://github.com/songconmaisaix31-design/echo-edge-training/pull/20) 一起交付。

## Windows 本机部署

准备 Windows x64、PowerShell 7、Python 3.13、Node.js 24 和已登录的 GitHub CLI。克隆仓库的 `main`，进入根目录。下面的 CPU FP32 配置可运行 Stanford 切面识别与 CardiacUDC 四腔结构分割。两个 Python 环境分别安装 API 与模型依赖。

```powershell
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$repo = (Get-Location).Path
$state = Join-Path $repo 'work/workstation'
New-Item -ItemType Directory -Force -Path $state | Out-Null
$env:ECHO_API_PYTHON = Join-Path $state 'api-venv/Scripts/python.exe'
$env:ECHO_INFERENCE_PYTHON = Join-Path $state 'model-venv/Scripts/python.exe'
python -m venv (Join-Path $state 'api-venv')
python -m venv (Join-Path $state 'model-venv')
& $env:ECHO_API_PYTHON -m pip install -r backend/requirements.txt
& $env:ECHO_INFERENCE_PYTHON -m pip install 'torch==2.12.1+cpu' 'torchvision==0.27.1+cpu' `
  --index-url https://download.pytorch.org/whl/cpu
& $env:ECHO_INFERENCE_PYTHON -m pip install -r requirements.txt
npm.cmd --prefix frontend ci
npm.cmd --prefix frontend run build

$repository = 'songconmaisaix31-design/echo-edge-training'
$stanford = Join-Path $state 'stanford'
$cardiac = Join-Path $state 'cardiacudc'
gh release download stanford-fp16-epoch10 -R $repository --dir $stanford --skip-existing `
  --pattern 'Stanford-A4C-PSAX-best.pt' --pattern 'Stanford-TensorRT-FP32-FP16-test.json'
gh release download cardiacudc-fourchamber-v1 -R $repository --dir $cardiac --skip-existing `
  --pattern 'CardiacUDC-FourChamber-UNet-epoch18.pt'

$database = Join-Path $state 'platform.db'
$env:ECHO_DATABASE_URL = 'sqlite:///' + ($database -replace '\\', '/')
$env:ECHO_MEDIA_ROOT = Join-Path $state 'media'
$env:ECHO_INFERENCE_SCRIPT = Join-Path $repo 'infer_stanford.py'
$env:ECHO_ENGINE_REPORT = Join-Path $stanford 'Stanford-TensorRT-FP32-FP16-test.json'
$env:ECHO_STANFORD_CHECKPOINT = Join-Path $stanford 'Stanford-A4C-PSAX-best.pt'
$env:ECHO_STRUCTURE_PYTHON = $env:ECHO_INFERENCE_PYTHON
$env:ECHO_CARDIACUDC_CHECKPOINT = Join-Path $cardiac 'CardiacUDC-FourChamber-UNet-epoch18.pt'
```

连接运行 EdgeTerminal 的 ESP32-S3。首次配置设备时按下文编译与烧录固件。用以下命令查找串口，再按实际端口设置 `ECHO_TERMINAL_PORT`。对库执行迁移，首次部署时创建普通操作员账号并在提示中设置自己的密码。之后在同一 PowerShell 会话启动，保留上方环境变量。

```powershell
& $env:ECHO_API_PYTHON -m serial.tools.list_ports -v
$env:ECHO_TERMINAL_PORT = 'COM4'
Push-Location backend
try {
    & $env:ECHO_API_PYTHON -m alembic upgrade head
    & $env:ECHO_API_PYTHON create_user.py operator operator
} finally {
    Pop-Location
}
./Start-CardioBridge.ps1
```

浏览器能打开 `http://127.0.0.1:5181/` 的登录页后，在 PowerShell 检查前端代理和硬件、模型配置。

```powershell
(Invoke-WebRequest 'http://127.0.0.1:5181/' -TimeoutSec 5).StatusCode
Invoke-RestMethod 'http://127.0.0.1:5181/api/v1/capabilities' -TimeoutSec 5
```

| 变量 | 值 |
| --- | --- |
| `ECHO_API_PYTHON` | 装好后端依赖的 `python.exe` 绝对路径 |
| `ECHO_DATABASE_URL` | SQLite URL，例如 `sqlite:///D:/echo-state/platform.db` |
| `ECHO_MEDIA_ROOT` | 影像原件和预览的本地目录 |
| `ECHO_INFERENCE_PYTHON` | 装好推理依赖的 `python.exe` 绝对路径 |
| `ECHO_INFERENCE_SCRIPT` | `infer_stanford.py` 绝对路径 |
| `ECHO_ENGINE_REPORT` | 发布报告的绝对路径 |
| `ECHO_STANFORD_CHECKPOINT` | CPU FP32 模式使用发布 checkpoint。使用 TensorRT FP16 时清除该变量。 |
| `ECHO_STRUCTURE_PYTHON` | 装好 PyTorch、PyAV、NumPy 和 Pillow 的 `python.exe` 绝对路径 |
| `ECHO_CARDIACUDC_CHECKPOINT` | `cardiacudc-fourchamber-v1` 的第 18 轮权重绝对路径 |
| `ECHO_TERMINAL_PORT` | 装有 EdgeTerminal 的串口，例如 `COM4` |

网页和 API 只监听本机。需要避开已占用端口时使用 `./Start-CardioBridge.ps1 -ApiPort 8036 -WebPort 5188`，并以对应网页端口检查 `/api/v1/capabilities`。首页响应 `200`，能力接口应显示 `Stanford 研究模型 · PyTorch CPU FP32`、`structureConfigured: true` 和 `terminalConfigured: true`。在「系统管理 → 模型状态」可查看结构分割接入状态，点击「刷新数据」重新读取。如果模型配置为未就绪，检查 Release 文件是否齐全、环境变量是否为绝对路径。如果发送失败，核对板上固件和串口号，并退出占用串口的调试工具。启动脚本会输出服务日志位置。

数据库、媒体原件和叠图保存在 `work/workstation/` 下。备份或迁移时停止写入，保留 SQLite 数据库和整个 `media/` 目录。模型和现场影像按各自授权保存。

## ESP32-S3 固件

工作站串口连接需要运行 `firmware/edge-terminal/EdgeTerminal.ino`。下面使用 Arduino CLI 1.6.0 和 Arduino-ESP32 3.3.12 为 ESP32-S3-DevKitC-1 N16R8 构建。仅在需要安装或更新固件时执行 `upload`，使用查到的实际串口号。引脚和 USB 转串口说明见[终端固件说明](../firmware/README.md)。

```powershell
$index = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
arduino-cli core update-index --additional-urls $index
arduino-cli core install esp32:esp32@3.3.12 --additional-urls $index
$sketch = './work/firmware/EdgeTerminal'
$output = './work/firmware/build_edgeterminal'
New-Item -ItemType Directory -Force $sketch | Out-Null
Copy-Item ./firmware/edge-terminal/* $sketch -Force
$board = 'esp32:esp32:esp32s3:FlashMode=dio,FlashSize=16M,CDCOnBoot=default,PSRAM=disabled,PartitionScheme=default'
arduino-cli compile --fqbn $board --output-dir $output $sketch
arduino-cli board list
$port = 'COM4'
arduino-cli upload --fqbn $board --port $port --input-dir $output $sketch
```

2026 年 11 月 15 日使用上述板型编译 EdgeTerminal，程序存储占用 375,393 字节，全局变量占用 25,192 字节。联调复测使用原先已烧录的 `COM4` 终端。

## 已验证的路径

在「AI 辅助诊断」选中检查，导入原视频，运行切面识别。分析记录关联检查及影像，并保存模型版本和输入摘要。检查创建者或管理员可将已有结果明确发送到终端。后端读取该任务的保存结果，通过 `STAT` 核对计数，发送 `HINT VIEW A4C`、`HINT VIEW PSAX` 或 `HINT VIEW UNKNOWN`，验证 `ACK` 序号。网页在本次发送后显示回执，刷新时读取已保存的分析结果。串口异常时保留分析任务结果，提示操作者核对设备。EdgeTerminal 的 `ACK` 确认命令接收。

2026 年 11 月 15 日在 D 盘从新建的 API 与 CPU 模型环境、新建的 SQLite 数据库和重新下载的 Release 权重启动工作站。通过 Chromium 和 Vite HTTP 代理登录，导入 53 帧公开教学视频。Stanford CPU FP32 模型抽样 8 帧，任务成功且刷新后读回。实际 `COM4` 返回 `ACK,27`，命令到回执耗时 22.930 ms。未登录发送请求返回 HTTP 401。公开样例 S3 的论文标注为 A4C，本次模型输出 PSAX。该结果用于核对链路与误分实例。[公开样例来源](https://pocusjournal.com/article/2021-06-01p10-12/)

团队双头模型处于训练阶段。#12 正式验收仍需冻结的双头模型、三类真实视频及完整联动记录。本次终端遥测带有 `MOCK` 标记，实测范围为指令和串口回执。

## 四腔结构分割

在「AI 影像分析」中选择正式检查与影像，切换到「心脏结构」。也可在「AI 辅助诊断」选择检查后打开「诊断证据」，或从检查、影像页面进入「打开四腔分割」。选择已确认属于 A4C 的帧，再点击「运行四腔分割」。`POST /api/v1/media/{id}/structure-jobs` 接收从 0 开始的源帧序号，校验 Release 权重的 SHA-256，以 256 × 256 灰度输入运行四腔 U-Net。结果保存源帧和时间戳、权重摘要及结构叠图。`GET /api/v1/analysis-jobs/{id}/overlay` 按检查访问权限返回叠图。检查创建者或管理员可以发送 `HINT MASK READY`，网页将在核对实际 `ACK` 后显示回执。

用上面的公开教学视频首帧复测。第 18 轮模型生成 9,330 个前景掩膜像素，网页成功加载叠图，刷新后读回叠图及原有切面结果。未登录读取叠图返回 401，越界帧返回 422。`COM4` 返回 `ACK,28`，命令到回执 25.416 ms。该结果用于验证数据流和设备指令。CardiacUDC 的发布范围为非商业研究，成人／混合队列的输出在儿童场景需要单独验证。[CardiacUDC Release](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/cardiacudc-fourchamber-v1)

## 大模型影像质量和异常征象复核

后端使用 OpenAI 兼容的 Chat Completions 接口提交已导入视频的时间采样帧。配置必须指定支持图片和 JSON 输出的模型。当前 DeepSeek 接入使用 `deepseek-flash`。在启动后端的同一 PowerShell 会话设置以下变量。示例复用本机已有的 DeepSeek 凭据环境变量。

```powershell
$env:ECHO_REVIEW_BASE_URL = 'https://api.deepseek.com'
$env:ECHO_REVIEW_MODEL = 'deepseek-flash'
$env:ECHO_REVIEW_API_KEY_ENV = 'ANTHROPIC_AUTH_TOKEN'
./Start-CardioBridge.ps1
```

`ECHO_REVIEW_API_KEY_ENV` 保存服务端读取凭据的环境变量名。默认名为 `ECHO_REVIEW_API_KEY`。使用其他兼容服务时，将上述基地址和模型名改为对应配置，并在服务进程环境中设置 API key。凭据只在后端读取，能力接口返回配置状态、模型名和配置说明。请求采用 HTTPS，连接超时为 15 秒，读取超时为 180 秒。

在「AI 影像分析」选择正式检查及其影像，切到「影像质量」或「异常征象」，点击对应的运行按钮。后端最多抽取 12 个时间采样帧，将图像和时间戳发送到配置的模型服务。模型按心脏超声复核结构输出结论、可见观察、解释、证据帧、置信程度自评、评估限制与医生复核建议。点击证据按钮可查看实际源帧。分析记录保存到 SQLite，刷新、切换页面或重新进入当前检查可以读回。下拉列表可查阅该影像同类任务的历史结果。

「AI 辅助诊断」的「AI 诊断结果」提供质量复核，「诊断证据」提供异常征象复核。首页和「系统管理」从 `/api/v1/capabilities` 的 `reviewConfigured`、`reviewModel`、`reviewDetail` 读取配置状态。一次真实任务成功后可确认服务及图像调用可用。

`POST /api/v1/media/{id}/review-jobs` 接收 `{"kind":"quality"}` 或 `{"kind":"findings"}`。结果沿用分析任务权限、持久化和历史读取接口，分别存为 `llm_quality` 和 `llm_findings`。服务端校验输出结构、任务类型和证据帧归属，证据时间戳取自导入帧索引。模型拒绝、鉴权错误、限流、超时和不合格响应记录为失败任务，页面显示失败原因。同一后端同时执行一个大模型复核任务。

静态采样只覆盖所提供片段的部分帧，动态功能、其他切面和完整检查仍需医生阅片。置信程度为模型自评，输出属于研究复核意见，临床有效性需独立评价。[DeepSeek 视觉输入](https://api-docs.deepseek.com/guides/vision/) · [JSON 输出](https://api-docs.deepseek.com/guides/json_mode/)

2026 年 11 月 15 日通过 Chromium、真实 HTTP 服务及隔离 SQLite 验证，使用 53 帧公开教学视频抽样 12 帧调用 DeepSeek。质量复核返回「评估受限」，征象复核返回「无法判断」，均附观察、实际帧索引和限制说明。结果刷新与跨检查隔离通过，未登录提交返回 401，非法任务类型返回 422。此次验证覆盖功能链路，输出的医学判断仍需临床专家独立评价。
