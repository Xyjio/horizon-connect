# Builds build\HorizonConnect.dll with Zig's C compiler.
#   .\tools\build.ps1                  (zig on the PATH)
#   .\tools\build.ps1 -Zig C:\zig\zig.exe
param([string]$Zig = 'zig')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build'
New-Item -ItemType Directory $out -Force | Out-Null
& $Zig cc -target x86_64-windows-gnu -shared -O2 -Wall -o (Join-Path $out 'HorizonConnect.dll') (Join-Path $root 'plugin\horizonconnect.c') `
  -lws2_32 -lwinhttp -lole32 -luser32 -lshell32 -ladvapi32 -lgdi32
if ($LASTEXITCODE) { throw "zig cc failed ($LASTEXITCODE)" }
Get-Item (Join-Path $out 'HorizonConnect.dll') | Select-Object FullName, Length
