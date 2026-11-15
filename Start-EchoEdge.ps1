param([int]$ApiPort = 8019, [int]$WebPort = 5181, [ValidateSet('127.0.0.1','0.0.0.0')][string]$Bind = '127.0.0.1')
$ErrorActionPreference = 'Stop'
if ($ApiPort -eq $WebPort) { throw 'API and Web ports must be different.' }
foreach ($port in @($ApiPort, $WebPort)) {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port)
    try { $listener.Start() } catch { throw "Port $port is occupied. Choose another port." }
    finally { $listener.Stop() }
}
$state = Join-Path (Split-Path -Parent $PSScriptRoot) 'local-services'
New-Item -ItemType Directory -Path $state -Force | Out-Null
$run = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$api = Start-Process pwsh -WorkingDirectory $PSScriptRoot -ArgumentList "-NoProfile -File Start-StanfordBackend.ps1 -Port $ApiPort -Bind $Bind" -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $state "$run-api.out") -RedirectStandardError (Join-Path $state "$run-api.err")
$deadline = (Get-Date).AddSeconds(15)
do {
    if ($api.HasExited) { throw "Backend exited. Check $state/$run-api.err" }
    try { $null = Invoke-RestMethod "http://127.0.0.1:$ApiPort/api/v1/capabilities" -TimeoutSec 2; break }
    catch { Start-Sleep -Milliseconds 300 }
} while ((Get-Date) -lt $deadline)
$null = Invoke-RestMethod "http://127.0.0.1:$ApiPort/api/v1/capabilities" -TimeoutSec 2
$env:ECHO_API_TARGET = "http://127.0.0.1:$ApiPort"
$web = Start-Process npm.cmd -WorkingDirectory (Join-Path $PSScriptRoot 'frontend') -ArgumentList "run dev -- --port $WebPort --strictPort" -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $state "$run-web.out") -RedirectStandardError (Join-Path $state "$run-web.err")
Write-Output "Web http://127.0.0.1:$WebPort/  API http://127.0.0.1:$ApiPort/  Logs $state  Launcher PIDs $($api.Id), $($web.Id)"
