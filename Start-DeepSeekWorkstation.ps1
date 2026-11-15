param(
    [Parameter(Mandatory)][string]$RuntimePath,
    [int]$ApiPort = 8036,
    [int]$WebPort = 5192,
    [ValidateSet('127.0.0.1','0.0.0.0')][string]$Bind = '0.0.0.0',
    [string]$KeyEnvironment = 'ANTHROPIC_AUTH_TOKEN',
    [string]$TerminalPort = 'COM4'
)
$ErrorActionPreference = 'Stop'
$runtime = (Resolve-Path -LiteralPath $RuntimePath).Path
if (![Environment]::GetEnvironmentVariable($KeyEnvironment)) { throw "请在本机会话中设置凭据环境变量 $KeyEnvironment。" }
$files = @{
    ECHO_API_PYTHON = Join-Path $runtime 'api-venv/Scripts/python.exe'
    ECHO_INFERENCE_PYTHON = Join-Path $runtime 'deployment-check/model-venv/Scripts/python.exe'
    ECHO_INFERENCE_SCRIPT = Join-Path $PSScriptRoot 'infer_stanford.py'
    ECHO_ENGINE_REPORT = Join-Path $runtime 'models/Stanford-TensorRT-FP32-FP16-test.json'
    ECHO_STANFORD_CHECKPOINT = Join-Path $runtime 'models/Stanford-A4C-PSAX-best.pt'
    ECHO_CARDIACUDC_CHECKPOINT = Join-Path $runtime 'models/cardiacudc-fourchamber-v1/CardiacUDC-FourChamber-UNet-epoch18.pt'
}
foreach ($name in $files.Keys) {
    if (!(Test-Path -LiteralPath $files[$name] -PathType Leaf)) { throw "找不到 $name 对应的文件：$($files[$name])" }
}
$database = Join-Path $runtime 'integration.db'
$media = Join-Path $runtime 'media'
if (!(Test-Path -LiteralPath $database -PathType Leaf)) { throw "找不到既有数据库：$database" }
if (!(Test-Path -LiteralPath $media -PathType Container)) { throw "找不到既有影像目录：$media" }
foreach ($name in $files.Keys) { [Environment]::SetEnvironmentVariable($name, $files[$name], 'Process') }
$env:ECHO_STRUCTURE_PYTHON = $env:ECHO_INFERENCE_PYTHON
$env:ECHO_DATABASE_URL = 'sqlite:///' + $database.Replace('\','/')
$env:ECHO_MEDIA_ROOT = $media
$env:ECHO_TERMINAL_PORT = $TerminalPort
$env:ECHO_REVIEW_BASE_URL = 'https://api.deepseek.com'
$env:ECHO_REVIEW_MODEL = 'deepseek-flash'
$env:ECHO_REVIEW_API_KEY_ENV = $KeyEnvironment
& (Join-Path $PSScriptRoot 'Start-EchoEdge.ps1') -ApiPort $ApiPort -WebPort $WebPort -Bind $Bind
