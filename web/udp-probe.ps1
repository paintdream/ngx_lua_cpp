<#
udp-probe.ps1 - UDP smoke test for the ngx_lua_cpp demo center (Demo 4/5)

Sends datagrams to the UDP listeners started by demo/udp_echo_server.lua
(owned by the C++ instance -- nginx's stream module cannot do "listen ... udp"
on Windows, see web/nginx.conf):

  port 9000  ngx_lua_cpp echo    (replies "pong <payload> lua_cpp=true ...")
  port 9001  plain Lua hello     (replies "hello-udp")

Usage:
  powershell -ExecutionPolicy Bypass -File web/udp-probe.ps1
  powershell -ExecutionPolicy Bypass -File web/udp-probe.ps1 -Port 9000 -Count 5
  powershell -ExecutionPolicy Bypass -File web/udp-probe.ps1 -Expect "hello-udp"

Exit code 0 = all probes OK, 1 = any probe failed.
#>
param(
    [string]$DstHost = "127.0.0.1",
    [int]$Port = 9000,
    [string]$Payload = "hello-udp",
    [int]$Count = 3,
    [int]$TimeoutMs = 3000,
    [string]$Expect = "",      # substring the reply must contain
    [switch]$All               # probe both 9000 (ngx_lua_cpp) and 9001 (plain Lua)
)

$ErrorActionPreference = "Stop"

function Send-UdpProbe {
    param([string]$DstHost, [int]$DstPort, [string]$Data, [int]$TimeoutMs)

    $client = New-Object System.Net.Sockets.UdpClient
    try {
        $client.Client.ReceiveTimeout = $TimeoutMs
        $client.Connect($DstHost, $DstPort)
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Data)
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $client.Send($bytes, $bytes.Length) | Out-Null

        $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $reply = $client.Receive([ref]$ep)
        $sw.Stop()

        return @{
            ok      = $true
            reply   = [System.Text.Encoding]::UTF8.GetString($reply)
            rtt_ms  = $sw.Elapsed.TotalMilliseconds
            from    = $ep.ToString()
        }
    } catch {
        return @{ ok = $false; error = $_.Exception.Message }
    } finally {
        $client.Close()
    }
}

$targets = @()
if ($All) {
    $targets += @{ Port = 9000; Payload = "ngx-lua-cpp-udp"; Expect = "lua_cpp=true" }
    $targets += @{ Port = 9001; Payload = "x";               Expect = "hello-udp" }
} else {
    $targets += @{ Port = $Port; Payload = $Payload; Expect = $Expect }
}

$failed = 0
foreach ($t in $targets) {
    foreach ($i in 1..$Count) {
        $r = Send-UdpProbe $DstHost $t.Port $t.Payload $TimeoutMs
        if ($r.ok) {
            $match = "n/a"
            if ($t.Expect) { $match = if ($r.reply.Contains($t.Expect)) { "OK" } else { "MISMATCH" } }
            Write-Host ("[{0}:{1} #{2}] OK  rtt={3,6:N1} ms  reply='{4}'  match={5}" -f $DstHost, $t.Port, $i, $r.rtt_ms, $r.reply, $match)
            if ($t.Expect -and -not $r.reply.Contains($t.Expect)) { $failed++ }
        } else {
            Write-Host ("[{0}:{1} #{2}] FAIL {3}" -f $DstHost, $t.Port, $i, $r.error)
            $failed++
        }
    }
}

if ($failed -gt 0) {
    Write-Host "UDP probe FAILED ($failed failures). Is the demo center running with the new OpenResty?"
    exit 1
}
Write-Host "UDP probe OK."
exit 0
