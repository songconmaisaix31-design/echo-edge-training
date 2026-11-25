# Start-CardioBridge-Lan.ps1
#
# 和 Start-StanfordBackend.ps1 的区别只有一个: 绑 0.0.0.0。
# 床旁终端是另一台设备, 它只能通过局域网地址访问后端, 127.0.0.1 对它是不可达的。
# 其余环境与 integration-e2e 那套部署完全一致, 避免"重启后模型变不可用"。

param(
    [int]$ApiPort = 8036,
    [string]$Bind = '0.0.0.0'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot          # work/
$e2e = Join-Path $root 'integration-e2e'

if (!(Test-Path $e2e)) { throw "找不到部署目录 $e2e" }

$env:ECHO_API_PYTHON          = Join-Path $e2e 'api-venv/Scripts/python.exe'
$env:ECHO_INFERENCE_PYTHON    = Join-Path $e2e 'deployment-check/model-venv/Scripts/python.exe'
$env:ECHO_STRUCTURE_PYTHON    = $env:ECHO_INFERENCE_PYTHON
$env:ECHO_INFERENCE_SCRIPT    = Join-Path $root 'echo-edge-integration/infer_stanford.py'
$env:ECHO_ENGINE_REPORT       = Join-Path $e2e 'models/Stanford-TensorRT-FP32-FP16-test.json'
$env:ECHO_STANFORD_CHECKPOINT = Join-Path $e2e 'models/Stanford-A4C-PSAX-best.pt'
$env:ECHO_CARDIACUDC_CHECKPOINT = Join-Path $e2e 'models/cardiacudc-fourchamber-v1/CardiacUDC-FourChamber-UNet-epoch18.pt'
$env:ECHO_DATABASE_URL        = 'sqlite:///' + ((Join-Path $e2e 'integration.db') -replace '\\', '/')
$env:ECHO_MEDIA_ROOT          = Join-Path $e2e 'media'

if (!(Test-Path $env:ECHO_API_PYTHON)) { throw "找不到 API 解释器 $env:ECHO_API_PYTHON" }

$state = Join-Path $root 'local-services'
New-Item -ItemType Directory -Path $state -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'

$api = Start-Process -FilePath $env:ECHO_API_PYTHON `
    -ArgumentList @('-m', 'uvicorn', 'main:app', '--host', $Bind, '--port', "$ApiPort") `
    -WorkingDirectory (Join-Path $PSScriptRoot 'backend') `
    -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $state "$stamp-lan-api.out") `
    -RedirectStandardError  (Join-Path $state "$stamp-lan-api.err")

$deadline = (Get-Date).AddSeconds(25)
$probe = "http://127.0.0.1:$ApiPort/api/v1/capabilities"
do {
    if ($api.HasExited) { throw "后端启动即退出, 看 $state/$stamp-lan-api.err" }
    try { $caps = Invoke-RestMethod $probe -TimeoutSec 2; break } catch { Start-Sleep -Milliseconds 400 }
} while ((Get-Date) -lt $deadline)

if (!$caps) { throw "后端 25 秒内没有就绪" }

$lan = Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.AddressState -eq 'Preferred' } |
    Select-Object -ExpandProperty IPAddress

Write-Output "API   http://127.0.0.1:$ApiPort/  (bind $Bind)"
foreach ($ip in $lan) {
    Write-Output "LAN   http://${ip}:$ApiPort/"
}
Write-Output "PID   $($api.Id)  日志 $state/$stamp-lan-api.*"
Write-Output "CAPS  $($caps | ConvertTo-Json -Compress)"
