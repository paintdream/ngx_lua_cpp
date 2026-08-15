<#
run-server.ps1 - ngx_lua_cpp demo center helper

Syncs the canonical config/UI into the web/run nginx prefix and manages the
server process. All paths are resolved relative to this script, so it works
from any working directory.

Usage:
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1             # start (or restart)
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Stop       # stop
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Restart    # stop, sync, start
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -NginxExe C:\path\to\nginx.exe

The script copies web/nginx.conf and web/index.html into web/run (mime.types
is copied from the nginx install on first run), then starts
`nginx.exe -p web/run -c conf/nginx.conf` and probes http://localhost:8080/api/info.
#>
param(
    [switch]$Stop,
    [switch]$Restart,
    [string]$NginxExe = "C:\Tools\WinNMP\bin\nginx-1.7.7\nginx.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path   # this script's dir = web/
$run  = Join-Path $repo "run"
$pidFile = Join-Path $run "logs/nginx.pid"

function Invoke-Nginx([string[]]$NginxArgs) {
    & $NginxExe @NginxArgs
    if ($LASTEXITCODE -ne 0) {
        throw "nginx exited with code $LASTEXITCODE"
    }
}

# ---------------------------------------------------------------- stop
if ($Stop -or $Restart) {
    if (Get-Process -Name "nginx" -ErrorAction SilentlyContinue) {
        Write-Host "Stopping nginx..."
        if (Test-Path $pidFile) {
            Invoke-Nginx @("-p", $run, "-c", "conf/nginx.conf", "-s", "stop")
            # graceful shutdown can take a few seconds (keepalive timeouts)
            $deadline = (Get-Date).AddSeconds(10)
            while ((Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 500
                if (-not (Get-Process -Name "nginx" -ErrorAction SilentlyContinue)) { break }
            }
        }
        if (Get-Process -Name "nginx" -ErrorAction SilentlyContinue) {
            Stop-Process -Name "nginx" -Force
            Write-Host "nginx force-stopped."
        } else {
            Write-Host "nginx stopped."
        }
    }
    if ($Stop) { return }
}

# ---------------------------------------------------------------- sync files
New-Item -ItemType Directory -Force -Path (Join-Path $run "conf"), (Join-Path $run "logs"), (Join-Path $run "html") | Out-Null
Copy-Item (Join-Path $repo "nginx.conf") (Join-Path $run "conf/nginx.conf") -Force
Copy-Item (Join-Path $repo "index.html") (Join-Path $run "html/index.html") -Force

$mime = Join-Path $run "conf/mime.types"
if (-not (Test-Path $mime)) {
    $mimeSrc = Join-Path (Split-Path -Parent $NginxExe) "conf/mime.types"
    if (-not (Test-Path $mimeSrc)) {
        throw "mime.types not found at $mimeSrc - pass -NginxExe to point at your OpenResty install"
    }
    Copy-Item $mimeSrc $mime
    Write-Host "Copied mime.types from $mimeSrc"
}

# ---------------------------------------------------------------- start
Write-Host "Starting nginx (prefix: $run)..."
Start-Process -FilePath $NginxExe -ArgumentList "-p", $run, "-c", "conf/nginx.conf" `
    -WorkingDirectory (Split-Path -Parent $NginxExe) -WindowStyle Hidden
Start-Sleep -Seconds 1

try {
    $info = Invoke-RestMethod -Uri "http://127.0.0.1:8080/api/info" -TimeoutSec 5
    $state = if ($info.ngx_lua_cpp_running) { "ngx_lua_cpp running" } else { "ngx_lua_cpp STOPPED" }
    Write-Host "OK: demo center at http://localhost:8080 ($state, nginx/$($info.nginx_version))"
} catch {
    Write-Host "nginx started but /api/info is not reachable yet: $($_.Exception.Message)"
    Write-Host "Check the error log: $run/logs/error.log"
}
