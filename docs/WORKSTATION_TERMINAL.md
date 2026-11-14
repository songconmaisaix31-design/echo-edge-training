# 工作站、切面模型与终端联调

PR #13 和 #14 以同一提交 `3fcdc64` 为头。工作站在包含 EdgeTerminal 的 `main` 上合入一次。

## 启动

安装 `backend/requirements.txt` 中的后端依赖，在 `frontend/` 执行 `npm ci`。按 README 配置模型环境，准备发布报告和与之匹配的权重或引擎。在 Windows 上从仓库根目录设置以下环境变量。数据库、影像和模型目录使用获准的本地位置。

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

首次启动先在 `backend/` 执行 `& $env:ECHO_API_PYTHON -m alembic upgrade head` 和 `& $env:ECHO_API_PYTHON create_user.py operator operator`，设置账号密码。回到根目录运行 `./Start-EchoEdge.ps1`。默认 Web 与 API 只监听本机。串口由后端打开，使用前退出占用该串口的调试工具。

## 已验证的路径

在「AI 辅助诊断」选中检查，导入原视频，运行切面识别。分析记录关联检查及影像，并保存模型版本和输入摘要。检查创建者或管理员可将已有结果明确发送到终端。后端读取该任务的保存结果，通过 `STAT` 核对计数，发送 `HINT VIEW A4C`、`HINT VIEW PSAX` 或 `HINT VIEW UNKNOWN`，验证 `ACK` 序号。网页显示接收回执。串口异常时保留分析任务结果，提示操作者核对设备。EdgeTerminal 的 `ACK` 确认命令接收。

2026 年 11 月 15 日在 D 盘隔离环境使用本机 Chromium、Vite HTTP 代理、FastAPI、SQLite、已发布的 Stanford CPU FP32 权重和实际 `COM4` 测试。导入 53 帧公开教学视频，模型抽样 8 帧，任务成功且刷新后读回。终端返回 `ACK,22`，命令到回执耗时 23.066 ms。未登录发送请求返回 HTTP 401。公开样例 S3 的论文标注为 A4C，本次模型输出 PSAX。该结果用于核对链路与误分实例。[公开样例来源](https://pocusjournal.com/article/2021-06-01p10-12/)

团队双头模型处于训练阶段。#12 正式验收仍需冻结的双头模型、三类真实视频及完整联动记录。本次终端遥测带有 `MOCK` 标记，实测范围为指令和串口回执。

## 四腔结构分割

在「AI 辅助诊断 → 诊断证据」选择已确认属于 A4C 的视频帧。`POST /api/v1/media/{id}/structure-jobs` 接收从 0 开始的源帧序号，校验 Release 权重的 SHA-256，以 256 × 256 灰度输入运行四腔 U-Net。结果保存源帧和时间戳、权重摘要及结构叠图。`GET /api/v1/analysis-jobs/{id}/overlay` 按检查访问权限返回叠图。检查创建者或管理员可以发送 `HINT MASK READY`，网页将在核对实际 `ACK` 后显示回执。

用上面的公开教学视频首帧复测。第 18 轮模型生成 9,330 个前景掩膜像素，网页成功加载叠图，刷新后读回叠图及原有切面结果。未登录读取叠图返回 401，越界帧返回 422。`COM4` 返回 `ACK,25`，命令到回执 25.050 ms。该结果用于验证数据流和设备指令。CardiacUDC 的发布范围为非商业研究，成人／混合队列的输出在儿童场景需要单独验证。[CardiacUDC Release](https://github.com/songconmaisaix31-design/echo-edge-training/releases/tag/cardiacudc-fourchamber-v1)
