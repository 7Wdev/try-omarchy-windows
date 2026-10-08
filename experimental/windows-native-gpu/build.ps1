param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'build'))
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ Build Tools and the Windows SDK.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio C++ toolchain found.' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
# Fixed compiler invocation inside one cmd shell; no filesystem deletion/move.
# Reject shell metacharacters before constructing the quoted batch file.
foreach ($path in @($dev, $PSScriptRoot, $output)) {
    if ($path -match '["%&|<>^\r\n]') { throw 'Build paths contain unsupported shell characters.' }
}
$batch = Join-Path $output 'build-native-gpu.cmd'
@"
@echo off
call "$dev" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cd /d "$output"
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\wire_test.cpp" /Fe:wire-test.exe
if errorlevel 1 exit /b 1
wire-test.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\driver_wire_test.cpp" /Fe:driver-wire-test.exe
if errorlevel 1 exit /b 1
driver-wire-test.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /DUNICODE /D_UNICODE "$PSScriptRoot\driver_bridge.cpp" /Fe:driver-bridge.exe /link d3d12.lib dxgi.lib gdi32.lib ws2_32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\context_native_probe.cpp" /Fe:context-native-probe.exe /link dxgi.lib gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\queue_dependency_probe.cpp" /Fe:queue-dependency-probe.exe /link dxgi.lib gdi32.lib WinHvPlatform.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /DUNICODE /D_UNICODE "$PSScriptRoot\native_gpu.cpp" /Fe:native-gpu.exe /link d3d12.lib dxgi.lib user32.lib gdi32.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $env:ComSpec /d /c $batch
if ($LASTEXITCODE -ne 0) { throw "Native GPU build failed ($LASTEXITCODE)." }
