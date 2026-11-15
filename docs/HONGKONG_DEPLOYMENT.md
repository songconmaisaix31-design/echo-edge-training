# 香港工作站部署

公网工作站地址为 <https://echo.davidwang.space/>。部署在香港 Ubuntu 24.04 服务器，使用 2 核 CPU、4 GB 内存。Nginx 提供 HTTPS 和前端静态文件，将 `/api/` 原样代理至 `127.0.0.1:8036`。FastAPI 使用单个 Uvicorn worker，SQLite、原视频、抽帧和叠图保存在服务器持久目录。

账号由管理员创建。当前操作员为 `integration-operator`，密码通过私密交付提供，仓库不保存密码、API key、数据库或会话。

## 运行目录

| 路径 | 用途 |
| --- | --- |
| `/opt/echo-edge/src` | 工作站和模型推理源码 |
| `/opt/echo-edge/src/frontend/dist` | 构建后的网页 |
| `/opt/echo-edge/venvs/api` | Python 3.12 API 环境，使用 `backend/requirements.txt` |
| `/opt/echo-edge/venvs/model` | PyTorch CPU 环境，使用 `deploy/model-requirements.txt` |
| `/opt/echo-edge/models` | Stanford 和 CardiacUDC 发布权重 |
| `/opt/echo-edge/data/platform.db` | SQLite 数据库 |
| `/opt/echo-edge/data/media` | 原视频、帧索引、图像和分割叠图 |
| `/opt/echo-edge/data/tmp` | 模型运行时临时文件 |
| `/opt/echo-edge/work` | 安装包和安装日志 |
| `/etc/echo-edge.env` | 后端环境变量，root 所有、权限 `0600` |
| `/etc/systemd/system/echo-edge.service` | 开机启动、异常退出重启和目录权限约束 |
| `/etc/nginx/sites-available/echo.davidwang.space` | 独立站点配置 |

API 与模型各使用一个 venv，分别安装 PyAV 15.1.0 和 19.0.2。CPU 模型环境固定 PyTorch `2.12.1+cpu` 和 TorchVision `0.27.1+cpu`。工作站视频接口接受 MP4、AVI、MOV、WebM；Linux CPU 部署依赖覆盖实际视频推理导入路径，不安装训练用 DICOM JPEG 解码插件。

## 安装

先核对服务器指纹、可用空间和已有站点。使用已验证的 SSH 身份连接，保留其他站点配置。以下路径约定部署根目录为 `/opt/echo-edge`。

从 D 盘仓库构建前端并打包已提交源码：

```powershell
npm.cmd --prefix frontend ci
npm.cmd --prefix frontend run build
New-Item -ItemType Directory -Force work/cloud-deploy | Out-Null
git archive --format=tar.gz --output=work/cloud-deploy/source.tar.gz HEAD
tar -czf work/cloud-deploy/frontend-dist.tar.gz -C frontend dist
```

从仓库 Release 下载 `Stanford-A4C-PSAX-best.pt`、`Stanford-TensorRT-FP32-FP16-test.json` 和 `CardiacUDC-FourChamber-UNet-epoch18.pt`。具体 Release 下载命令见[模型部署说明](WORKSTATION_TERMINAL.md)。服务器模型目录应为：

- `/opt/echo-edge/models/Stanford-A4C-PSAX-best.pt`
- `/opt/echo-edge/models/Stanford-TensorRT-FP32-FP16-test.json`
- `/opt/echo-edge/models/cardiacudc-fourchamber-v1/CardiacUDC-FourChamber-UNet-epoch18.pt`

上传源码压缩包、前端压缩包和模型，分别展开至上表路径。前端压缩包包含 `dist` 目录，应展开到 `/opt/echo-edge/src/frontend`。以 root 执行：

```bash
bash /opt/echo-edge/src/deploy/install-linux.sh
```

该脚本安装 Python venv 支持和固定依赖，创建 `echo-edge` 服务用户及数据目录，执行 Alembic 迁移，并安装 systemd unit。数据库迁移预期为 `0006`。脚本不会覆盖已有账号或启动尚未配置凭据的服务。

在持有已配置 DeepSeek 凭据的 PowerShell 会话，通过 SSH 标准输入创建服务器环境文件。将 `<服务器>` 替换为已验证的 SSH 别名或主机：

```powershell
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
if (!$env:ANTHROPIC_AUTH_TOKEN) { throw '缺少已配置的 DeepSeek 凭据' }
@{review_key=$env:ANTHROPIC_AUTH_TOKEN} | ConvertTo-Json -Compress |
    ssh <服务器> 'python3 /opt/echo-edge/src/deploy/configure-environment.py'
```

环境生成工具以独占创建方式写入 `/etc/echo-edge.env`，存在时立即失败。它设置实际模型路径、DeepSeek 地址和模型名、HTTPS Cookie，并生成独立随机 `ECHO_PANEL_KEY`。密钥不会输出到终端。云端不设置本机串口 `COM4`。

首次部署通过已有账号工具创建操作员；密码由交互提示接收：

```bash
cd /opt/echo-edge/src/backend
runuser -u echo-edge -- env ECHO_DATABASE_URL=sqlite:////opt/echo-edge/data/platform.db /opt/echo-edge/venvs/api/bin/python create_user.py integration-operator operator
systemctl enable --now echo-edge.service
```

本次部署迁移了本机既有操作员的加盐密码摘要，使用 `deploy/import-operator.py` 从 SSH 标准输入接收账号名称和摘要。没有迁移本机数据库或登录会话。新服务器通常使用上面的交互账号创建命令即可。

将域名 `echo.davidwang.space` 的 A 记录指向部署服务器。首次签发证书使用 `deploy/echo.davidwang.space.nginx.conf`，为该域名单独建立 `sites-available` 文件及 `sites-enabled` 链接，执行 `nginx -t` 后重新加载 Nginx。随后签发证书：

```bash
certbot --nginx -d echo.davidwang.space --non-interactive --redirect
```

此命令沿用服务器已存在的 Certbot 账号。证书成功后使用 `deploy/echo.davidwang.space.https.conf` 作为最终站点配置，再执行 `nginx -t` 和 `systemctl reload nginx`。最终配置启用 HTTPS 跳转、70 MiB 请求体上限、180 秒收包超时、300 秒后端响应超时及 JS/CSS/JSON 压缩。应用仍执行自身 64 MiB 视频上限。

## 维护与验证

```bash
systemctl status echo-edge --no-pager
journalctl -u echo-edge -n 80 --no-pager
nginx -t
curl -fsS https://echo.davidwang.space/api/v1/capabilities
```

能力接口反映配置状态。模型验收必须实际导入视频，检查分析任务 `state=succeeded`，并读取叠图、证据帧和刷新后的历史记录。

2026 年 11 月 15 日通过系统 Chrome 的实际网页入口验证了以下结果。使用的源代码为 `90206786da05453255bbe240e03cf850a769f134`，部署配置由本目录提供。

| 验证项 | 实际结果 |
| --- | --- |
| HTTPS 登录 | `integration-operator` 登录成功；会话 Cookie 为 Secure、HttpOnly、SameSite=Strict |
| 设置状态 | 结构分割显示“已接入”，DeepSeek 模型配置可读取 |
| 视频导入 | 通过网页文件选择器导入 53 帧公开教学视频，保存为云端检查 #1 |
| 四腔分割 | 任务 #2 成功，第 2 帧、100 ms、模型第 18 轮；前景像素 7426，叠图实际加载 |
| 影像质量 | DeepSeek `deepseek-flash` 任务 #3 成功，实际发送 12 个采样帧 |
| 异常征象 | DeepSeek `deepseek-flash` 任务 #4 成功，实际发送 12 个采样帧 |
| 证据与刷新 | 两项 DeepSeek 证据帧实际加载；整页刷新后四腔和两项复核结果均读回 |
| 服务重启 | `systemctl restart echo-edge` 后重新登录 Chrome，任务 #1—#4、叠图与两项复核结果继续读回；未重新调用模型 |
| 未登录保护 | 身份、检查分析历史、四腔叠图、设备面板状态均返回 401 |
| 浏览器执行 | 未出现页面脚本异常 |

API 与模型 venv 的 `pip check` 均通过；`nginx -t`、Python 编译检查、`node --check deploy/verify-live.cjs` 和 `git diff --check` 均通过。`echo-edge` 与 `certbot.timer` 已启用。

验收档案复用本机明确标记的既有联调记录，检查说明明确：公开教学视频与该档案的身份信息无关联。没有根据论文年龄推测或编造出生日期。公开视频来源为 [POCUS Journal 的 Video S3](https://pocusjournal.com/article/2021-06-01p10-12/)。

`deploy/verify-live.py` 在隐藏密码提示后启动真实 Chrome。`deploy/verify-live.cjs` 登录网页、读取设置、通过网页上传视频并点击三个模型入口。它使用实际后端，不拦截请求、不生成虚假结果、不截取屏幕。需要先安装 `frontend/package.json` 中的 Playwright，并让 Node 能找到该模块：

```powershell
$env:NODE_PATH = (Resolve-Path frontend/node_modules).Path
python deploy/verify-live.py --exam 1 --video 'D:/path/to/public-a4c.mp4'
```

已有成功结果的刷新验收使用 `--read-only --exam 1`。该模式不会上传影像或再次调用模型。验证报告写入忽略目录 `work/`，脚本结束时退出验证会话。重新创建验收检查时，可用 `--patient` 指定实际已有档案 JSON，或用 `--source-exam` 从本机已明确标记的联调档案读取既有字段；不得编造出生日期。公开教学视频与联调档案身份信息无关联，检查说明必须保留这一点。

更新代码时先停止 `echo-edge`，备份整个数据目录，再更新源码、前端和依赖并执行迁移。数据目录和环境文件必须保留。完成后启动服务，执行只读结果回读验证。不能让两个 Uvicorn 进程同时写入同一数据库。

## 服务器状态和限制

香港实例和云助手正常运行，SSH 可连接。2026 年 11 月 15 日部署验收时，根盘可用空间约 36 GB，同机的 `bca.davidwang.space` 返回 HTTPS 200。

- 既有 Anvil 服务的历史缓存未设置持久化上限，需要其维护者安排容量限制。Echo 部署未修改该服务。
- 当前服务器在北京时间 **2026 年 11 月 20 日 00:00** 到期，继续使用需要续费。
- 云端不直接访问电脑 USB 串口，未启用 ESP32 发送入口；公网模型验证不代表本地硬件通道已接入云端。
- SQLite 和影像目前位于单台服务器；尚未配置异地自动备份。
- 模型输出用于研究和医生复核，公开教学视频验收证明数据链路可执行，不提供临床性能结论。
