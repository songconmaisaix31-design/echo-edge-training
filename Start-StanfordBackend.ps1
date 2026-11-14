param([int]$Port = 8007)
$ErrorActionPreference = 'Stop'
$work = Split-Path -Parent $PSScriptRoot
$env:ECHO_DATABASE_URL = 'sqlite:///' + ((Resolve-Path (Join-Path $work 'platform.db')).Path -replace '\\','/')
$env:ECHO_MEDIA_ROOT = Join-Path $work 'platform-media'
$env:ECHO_INFERENCE_PYTHON = (Resolve-Path (Join-Path $work 'stanford-cpu-venv/Scripts/python.exe')).Path
$env:ECHO_INFERENCE_SCRIPT = (Resolve-Path (Join-Path $work 'echo-edge-model-release/infer_stanford.py')).Path
$env:ECHO_ENGINE_REPORT = (Resolve-Path (Join-Path $work 'stanford-fp16-epoch10/Stanford-TensorRT-FP32-FP16-test.json')).Path
$env:ECHO_STANFORD_CHECKPOINT = (Resolve-Path (Join-Path $work 'stanford-fp16-epoch10/Stanford-A4C-PSAX-best.pt')).Path
$apiPython = (Resolve-Path (Join-Path $work 'api-venv/Scripts/python.exe')).Path
Push-Location (Join-Path $PSScriptRoot 'backend')
try {
    & $apiPython -m uvicorn main:app --host 127.0.0.1 --port $Port
    if ($LASTEXITCODE -ne 0) { throw "Backend exited ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
