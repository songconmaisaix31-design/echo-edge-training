# 工作站离线 HTML 展示

导出工具从正在运行的工作站读取当前账号有权访问的真实数据，保存已渲染的页面和现有 CSS。导出结果为一个 `index.html`，包含解码帧、四腔叠图和已保存的模型结果，可直接打开，也可作为静态站点发布。

## 导出

准备 Node.js、系统 Chrome、已启动的工作站及有访问权限的账号。在仓库根目录安装前端依赖。通过当前进程的环境变量提供登录信息，避免将凭据写入脚本或仓库。

```powershell
npm.cmd --prefix frontend ci --ignore-scripts
$env:ECHO_EXPORT_ORIGIN = 'http://127.0.0.1:5192'
$env:ECHO_EXPORT_USER = Read-Host '导出账号'
$exportPassword = Read-Host '导出密码' -MaskInput
$env:ECHO_EXPORT_PASSWORD = $exportPassword
node tools/export-workstation-html.mjs work/html-export/index.html
Remove-Item Env:ECHO_EXPORT_PASSWORD
Remove-Variable exportPassword
```

程序只登录和读取数据，不提交分析、修改患儿档案、创建检查或导入影像。后端可能为登录和读取记录访问审计。导出前应完成需要保存的分析，导出期间保持检查内容稳定。

导出目录位于被 Git 忽略的 `work/` 下。生成的 HTML 包含实际检查数据，按原数据的共享范围保管和发布。Git 只保存导出工具与说明。

## 可读内容和交互

- 首页、患儿管理、检查管理、影像管理、AI 影像分析、多切面分析、AI 辅助诊断、专家协作、报告中心和系统管理。
- 患儿的基本资料、临床病史、检查记录、历史报告；检查的临床参数、超声测量、影像资料、AI 分析入口、审核记录和修改历史。
- 检查和影像切换、全部解码帧的时间轴及播放；播放时间来自真实帧时间戳。
- Stanford 已保存结果、全部质量与异常征象复核记录、证据帧和四腔分割历史叠图。
- 硬刷新后通过离线路由恢复当前检查、影像、模型面板和选择的大模型分析记录。

页面顶部始终显示导出时间及只读状态。上传、重新分析、连接检查、设备发送、保存、创建、编辑、账号操作及需要在线服务的下载入口被禁用。模型状态表示导出时在线工作站报告的配置。静态展示不会启动模型或后端服务。

导出文件不包含登录密码、登录账号、Cookie、API key、数据库文件或本机绝对路径。所有影像和样式内嵌；页面通过 Content Security Policy 禁止联网和表单提交。原视频接口未开放，影像播放使用工作站已经解码的源帧。

## Vercel 发布

当前展示地址为 [https://echo-edge-html.vercel.app/](https://echo-edge-html.vercel.app/)，项目为 `dwwww/echo-edge-html`。这是无需登录的只读 HTML 快照；可登录并运行新分析的工作站位于 [https://echo.davidwang.space/](https://echo.davidwang.space/)。两个站点分别发布，HTML 不保存工作站登录会话，也不会请求工作站 API。

以下流程已使用 Vercel CLI `56.3.1` 执行。先完成导出与本地验证，再从仓库根目录准备专门的发布目录，只放入最终 HTML 和 [静态站点配置](../deploy/vercel-html.json)。配置为页面设置禁止搜索索引、禁止 MIME 类型猜测和不发送来源网址的响应头。

```powershell
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
node tools/export-workstation-verify.mjs work/html-export/index.html
$exportCommit = git rev-parse HEAD
New-Item -ItemType Directory -Force work/html-vercel | Out-Null
Copy-Item -LiteralPath work/html-export/index.html -Destination work/html-vercel/index.html
Copy-Item -LiteralPath deploy/vercel-html.json -Destination work/html-vercel/vercel.json
Push-Location work/html-vercel
try {
    vercel.cmd link --yes --scope dwwww --project echo-edge-html --non-interactive
    if (Test-Path -LiteralPath .env.local) { Remove-Item -LiteralPath .env.local }
    vercel.cmd project inspect --scope dwwww --non-interactive
    vercel.cmd deploy --dry --format=json --scope dwwww --project echo-edge-html --non-interactive
} finally {
    Pop-Location
}
```

使用 CLI 已登录的目标团队凭据。项目检查应显示 `Dwwww / echo-edge-html`；预检文件列表应只有 `index.html` 和 `vercel.json`。静态站点不需要环境文件，CLI 生成的 `.env.local` 在发布目录中移除；`.vercel` 和 `.gitignore` 由 CLI 排除。不要将导出工具、数据库、账号凭据或其他中间文件复制到该目录。

先创建不绑定正式域名的生产部署，读取其实际内容，再将同一部署提升到正式地址：

```powershell
Push-Location work/html-vercel
try {
    $deployment = vercel.cmd deploy --prod --skip-domain --format=json --scope dwwww --project echo-edge-html --non-interactive --yes --meta "exportCommit=$exportCommit" | ConvertFrom-Json
    $deploymentUrl = $deployment.deployment.url
    if (!$deploymentUrl) { throw '部署没有返回地址' }
    vercel.cmd curl / --deployment $deploymentUrl -- --fail --silent --show-error --max-time 180 --output ../html-vercel-staged.html
    if ((Get-FileHash -LiteralPath index.html).Hash -ne (Get-FileHash -LiteralPath ../html-vercel-staged.html).Hash) { throw '线上内容与已验证的 HTML 不一致' }
    vercel.cmd promote $deploymentUrl --scope dwwww --non-interactive --yes
} finally {
    Pop-Location
}
node tools/export-workstation-verify.mjs https://echo-edge-html.vercel.app/
```

`vercel curl` 使用已核对的发布目录项目关联，并自动处理暂存部署的访问保护。正式地址由 `promote` 绑定到已检查的部署，不重新构建 HTML。

2026 年 11 月 15 日发布的生产部署为 `dpl_C2L3xJoLQBEPKvCqqpgiXB8UGXTn`，状态为 `READY`。导出工具版本为 `711da07583a8468b5a03727e90f33a0c00abd3b8`，在线工作站来源版本为 `90206786da05453255bbe240e03cf850a769f134`。发布文件为 16,474,287 字节；暂存生产地址返回 HTTP 200，读取内容与本地验收文件一致。

该单文件约 16.47 MB，包含全部导出的影像和页面数据。首次打开或硬刷新需要等待文件传输和解析，加载速度受访问网络影响。2026 年 11 月 15 日本轮公网 Chrome 验证在当前访问网络下，首次完整加载约 39 秒，6 次刷新各约 37—62 秒。普通页面切换、影像播放和结果选择使用已加载的内嵌数据。

## 验证

```powershell
node tools/export-workstation-verify.mjs work/html-export/index.html
node tools/export-workstation-verify.mjs https://echo-edge-html.vercel.app/
```

本地验证在断网的真实 Chrome 中打开 `file://` 文件。公网验证检查除文档导航外没有额外网络请求。两种验证均检查主要菜单、患儿与检查详情页、每条检查的每段影像、模型历史、分割叠图、证据帧、53 帧样例的播放与时间轴，以及刷新后的检查和影像归属。程序在页面错误、缺失资源或检查与影像不一致时立即失败。

公网验证默认给每次文档导航 120 秒，并记录实际导航耗时；交互断言的上限仍为 30 秒。需要按访问网络调整时，通过环境变量 `ECHO_EXPORT_NAVIGATION_TIMEOUT_MS` 设置 1000—600000 毫秒的导航上限。超时会直接失败，不自动重试。

当前工具按工作站已返回的列表和页面导出；病例列表沿用后端最近 200 条范围，列表分页沿用当前可见页面。若后续数据超过页面的一页容量，应先扩展列表分页导出与验证，再生成要求包含全部列表的快照。

2026 年 11 月 15 日，本地实际导出包含 4 份患儿资料、4 条检查、5 段影像共 219 帧和 29 条已保存分析任务；生成 110 个页面与结果状态，内嵌 225 张影像。真实 Chrome 的离线验证通过：全部菜单、患儿和检查详情、5 段影像及其分析历史、53 帧播放与时间轴、分割历史叠图和刷新保持正常；页面错误和额外网络请求均为 0。

同日，使用 `931599e7262b3481beb70079ff6764c9cd33b234` 的验证工具完成正式 Vercel 地址的公网 Chrome 验收，命令退出码为 0，报告为 `ok=true`、`mode=public-url`。10 个菜单、患儿详情 4 页、检查详情 6 页、5 段影像的模型结果和历史切换、逐段刷新保持、53 帧播放与时间轴、分割叠图全部通过。核对 110 个状态和 225 张图片，`externalRequests=0`、`pageErrors=0`；所有导航均在 120 秒上限内完成，交互断言保持 30 秒上限。
