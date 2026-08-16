<#
run-server.ps1 - ngx_lua_cpp demo center helper

Syncs the canonical config/UI into the web/run nginx prefix and manages the
server process. All paths are resolved relative to this script, so it works
from any working directory.

Usage:
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1             # start (or restart)
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Stop       # stop
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Restart    # stop, sync, start
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -TestUdp    # also run the UDP smoke test
  powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -NginxExe C:\path\to\nginx.exe

The script copies web/nginx.conf and web/index.html into web/run (mime.types
is copied from the nginx install on first run), then starts
`nginx.exe -p web/run -c conf/nginx.conf` and probes http://localhost:8080/api/info.

The default nginx is the official 64-bit OpenResty 1.27.1.2 installed next to
WinNMP (C:\Tools\OpenResty\openresty-1.27.1.2-win64). Note that UDP listeners
cannot be declared in nginx.conf on Windows at all -- nginx compiles
"listen ... udp" out on Windows (#if !(NGX_WIN32) in ngx_stream_core_module.c)
-- so the demo's UDP servers (ports 9000/9001) are owned by the ngx_lua_cpp
C++ instance (udp_listen/udp_recv/udp_send, see demo/udp_echo_server.lua).
Build the library for x64 to match this binary:

  cmake -S . -B build64 -A x64 -DLUA_LIBRARY=C:\Tools\OpenResty\openresty-1.27.1.2-win64\lua51.lib
  cmake --build build64 --config Debug
  (web/make-lua51-lib.ps1 generates lua51.lib from the shipped lua51.dll if missing)

Stop only touches nginx processes started with this prefix, so unrelated
nginx servers (including WinNMP's) are never killed.
#>
param(
    [switch]$Stop,
    [switch]$Restart,
    [switch]$TestUdp,
    [string]$NginxExe = "C:\Tools\OpenResty\openresty-1.27.1.2-win64\nginx.exe"
)

$ErrorActionPreference = "Stop"
# keep native stderr as plain stderr (best-effort `nginx -s stop` with a stale
# pid file writes to stderr; with PS 7.3+ this would otherwise become a
# terminating error and abort the script)
$PSNativeCommandUseErrorActionPreference = $false
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path   # this script's dir = web/
$run  = Join-Path $repo "run"
$pidFile = Join-Path $run "logs/nginx.pid"

# nginx processes belonging to THIS prefix (command line contains "-p <run dir>");
# never touch unrelated nginx servers (e.g. WinNMP's own).
function Get-OwnNginxPids {
    try {
        $runKey = ($run -replace '/', '\').ToLowerInvariant()
        $pids = Get-CimInstance Win32_Process -Filter "Name='nginx.exe'" -ErrorAction Stop |
            Where-Object { $_.CommandLine -and (($_.CommandLine -replace '/', '\').ToLowerInvariant() -like "*$runKey*") } |
            Select-Object -ExpandProperty ProcessId
        return @($pids)
    } catch {
        # CIM unavailable: fall back to the pid file, then to name matching
        if (Test-Path $pidFile) {
            $p = Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($p -and $p -match '^\d+$') { return @([int]$p) }
        }
        $all = @(Get-Process -Name "nginx" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
        return $all
    }
}

# ---------------------------------------------------------------- stop
if ($Stop -or $Restart) {
    $pids = Get-OwnNginxPids
    if ($pids.Count -gt 0) {
        Write-Host "Stopping nginx (pid $($pids -join ', '))..."
        # a stale pid file (crash / manual kill / test artifact) makes -s stop
        # fail; never abort on that, fall through to the force-kill below.
        # (PS 5.1 turns native stderr into terminating errors under
        # $ErrorActionPreference=Stop, hence the try/catch.)
        if (Test-Path $pidFile) {
            try {
                & $NginxExe -p $run -c conf/nginx.conf -s stop 2>$null
            } catch {
                # stale pid file - force-kill below
            }
            # graceful shutdown can take a few seconds (keepalive timeouts)
            $deadline = (Get-Date).AddSeconds(10)
            while ((Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 500
                if ((Get-OwnNginxPids).Count -eq 0) { break }
            }
        }
        $still = Get-OwnNginxPids
        if ($still.Count -gt 0) {
            Stop-Process -Id $still -Force -ErrorAction SilentlyContinue
            Write-Host "nginx force-stopped."
        } else {
            Write-Host "nginx stopped."
        }
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
    if ($Stop) { return }
}

# ---------------------------------------------------------------- sync files
New-Item -ItemType Directory -Force -Path (Join-Path $run "conf"), (Join-Path $run "logs"), (Join-Path $run "html"), (Join-Path $run "upload_tmp") | Out-Null
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
if ((Get-OwnNginxPids).Count -gt 0) {
    Write-Host "nginx already running for this prefix - use -Restart to restart it"
} else {
    Write-Host "Starting nginx (prefix: $run)..."
    Start-Process -FilePath $NginxExe -ArgumentList "-p", $run, "-c", "conf/nginx.conf" `
        -WorkingDirectory (Split-Path -Parent $NginxExe) -WindowStyle Hidden
    Start-Sleep -Seconds 1
}

try {
    $info = Invoke-RestMethod -Uri "http://127.0.0.1:8080/api/info" -TimeoutSec 5
    $state = if ($info.ngx_lua_cpp_running) { "ngx_lua_cpp running" } else { "ngx_lua_cpp STOPPED" }
    Write-Host "OK: demo center at http://localhost:8080 ($state, nginx/$($info.nginx_version))"
} catch {
    Write-Host "nginx started but /api/info is not reachable yet: $($_.Exception.Message)"
    Write-Host "Check the error log: $run/logs/error.log"
}

# ---------------------------------------------------------------- udp test
if ($TestUdp) {
    Write-Host "Probing UDP listeners (9000 = ngx_lua_cpp echo, 9001 = plain Lua)..."
    & powershell -ExecutionPolicy Bypass -File (Join-Path $repo "udp-probe.ps1") -All
}
