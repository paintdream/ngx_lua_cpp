<#
make-lua51-lib.ps1 - generate the LuaJIT import library for a Windows OpenResty

The official OpenResty Windows builds ship lua51.dll but no .lib, so C++/CMake
projects (like ngx_lua_cpp) cannot link against LuaJIT out of the box. This
script rebuilds lua51.lib from the DLL exports using the MSVC tools.

Usage:
  powershell -ExecutionPolicy Bypass -File web/make-lua51-lib.ps1
  powershell -ExecutionPolicy Bypass -File web/make-lua51-lib.ps1 -OpenRestyExe C:\Tools\OpenResty\openresty-1.27.1.2-win64\nginx.exe

Then configure the build with:
  cmake -S . -B build64 -A x64 -DLUA_LIBRARY=C:\Tools\OpenResty\openresty-1.27.1.2-win64\lua51.lib
  cmake --build build64 --config Debug
#>
param(
    [string]$OpenRestyExe = "C:\Tools\OpenResty\openresty-1.27.1.2-win64\nginx.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $OpenRestyExe)) {
    throw "nginx.exe not found at $OpenRestyExe - pass -OpenRestyExe"
}
$root  = Split-Path -Parent $OpenRestyExe
$dll   = Join-Path $root "lua51.dll"
$lib   = Join-Path $root "lua51.lib"
if (-not (Test-Path $dll)) {
    throw "lua51.dll not found next to $OpenRestyExe"
}
if (Test-Path $lib) {
    Write-Host "lua51.lib already exists at $lib (delete it to regenerate)"
    exit 0
}

# locate MSVC tools via vswhere
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found - is Visual Studio with C++ tools installed?"
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw "No Visual Studio C++ toolchain found"
}
$msvc = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $msvc) {
    throw "No MSVC toolset found under $vs\VC\Tools\MSVC"
}
$t = Get-ChildItem (Join-Path $msvc "bin") -Directory | Where-Object { $_.Name -match '^Hostx64\x64$' } | Select-Object -First 1 -ExpandProperty FullName
if (-not $t) {
    throw "No Hostx64/x64 toolchain found under $msvc"
}
$dumpbin = Join-Path $t "dumpbin.exe"
$libexe  = Join-Path $t "lib.exe"
if (-not (Test-Path $dumpbin) -or -not (Test-Path $libexe)) {
    throw "dumpbin.exe/lib.exe not found in $t"
}

# extract export names, write a canonical DEF, build the import lib
$def = Join-Path $env:TEMP "lua51.def"
$names = & $dumpbin /exports $dll 2>$null |
    Select-String '^\s+[0-9A-F]+\s+[0-9A-F]+\s+[0-9A-F]+\s+\S' |
    ForEach-Object { ($_.Line.Trim() -split '\s+')[3] } | Sort-Object -Unique
if (-not $names) {
    throw "no exports found in $dll"
}
@('LIBRARY lua51', 'EXPORTS') + $names | Set-Content -Path $def -Encoding ascii

& $libexe /def:$def /machine:x64 /out:$lib /nologo
if (-not (Test-Path $lib)) {
    throw "lib.exe failed to produce $lib"
}
Write-Host "Created $lib (LuaJIT import library)"
Remove-Item $def -ErrorAction SilentlyContinue