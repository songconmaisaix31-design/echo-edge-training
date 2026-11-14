param([int]$Port = 8007)
$ErrorActionPreference = 'Stop'
$work = Split-Path -Parent $PSScriptRoot

if (!$env:ECHO_DATABASE_URL) {
    $env:ECHO_DATABASE_URL = 'sqlite:///' + ((Join-Path $work 'platform.db') -replace '\\','/')
}
if (!$env:ECHO_MEDIA_ROOT) { $env:ECHO_MEDIA_ROOT = Join-Path $work 'platform-media' }
if (!$env:ECHO_INFERENCE_PYTHON -and !$env:ECHO_INFERENCE_SCRIPT -and !$env:ECHO_ENGINE_REPORT) {
    $env:ECHO_INFERENCE_PYTHON = (Resolve-Path (Join-Path $work 'stanford-cpu-venv/Scripts/python.exe')).Path
    $env:ECHO_INFERENCE_SCRIPT = (Resolve-Path (Join-Path $work 'echo-edge-model-release/infer_stanford.py')).Path
    $env:ECHO_ENGINE_REPORT = (Resolve-Path (Join-Path $work 'stanford-fp16-epoch10/Stanford-TensorRT-FP32-FP16-test.json')).Path
    if (!$env:ECHO_STANFORD_CHECKPOINT) {
        $env:ECHO_STANFORD_CHECKPOINT = (Resolve-Path (Join-Path $work 'stanford-fp16-epoch10/Stanford-A4C-PSAX-best.pt')).Path
    }
}
$apiPython = $env:ECHO_API_PYTHON
if (!$apiPython) { $apiPython = Join-Path $work 'api-venv/Scripts/python.exe' }
$apiPython = (Resolve-Path -LiteralPath $apiPython).Path
Push-Location (Join-Path $PSScriptRoot 'backend')
try {
    & $apiPython -m uvicorn main:app --host 127.0.0.1 --port $Port
    if ($LASTEXITCODE -ne 0) { throw "Backend exited ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
