param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'build'), [switch]$HighResolutionDiagnostic, [switch]$StreamingDiagnostic)
$ErrorActionPreference = 'Stop'
if ($StreamingDiagnostic) { $HighResolutionDiagnostic = $true }
if ($StreamingDiagnostic -and -not $PSBoundParameters.ContainsKey('OutputDirectory')) {
    $OutputDirectory = Join-Path $PSScriptRoot 'build-stream'
} elseif ($HighResolutionDiagnostic -and -not $PSBoundParameters.ContainsKey('OutputDirectory')) {
    $OutputDirectory = Join-Path $PSScriptRoot 'build-4k'
}
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
$profileDefine = if ($HighResolutionDiagnostic) { '/DWDDM_BRIDGE_4K_LAB=1' } else { '' }
if ($StreamingDiagnostic) { $profileDefine += ' /DWDDM_BRIDGE_STREAM_LAB=1' }
$captureTestDirectory = Join-Path $output ('capture-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $captureTestDirectory | Out-Null
@"
@echo off
call "$dev" -arch=x64 -host_arch=x64 >nul
if not "%errorlevel%"=="0" exit /b %errorlevel%
cd /d "$output"
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\wire_test.cpp" /Fe:wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\driver_wire_test.cpp" /Fe:driver-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
driver-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\sync_sharing_wire_test.cpp" /Fe:sync-sharing-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
sync-sharing-wire-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\native_worker_protocol_test.cpp" /Fe:native-worker-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
native-worker-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\cpu_aperture_test.cpp" /Fe:cpu-aperture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cpu-aperture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /DWDDM_BRIDGE_4K_LAB=1 "$PSScriptRoot\cpu_capacity_test.cpp" /Fe:cpu-capacity-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cpu-capacity-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\qmp_protocol_test.cpp" /Fe:qmp-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
qmp-protocol-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine /DUNICODE /D_UNICODE "$PSScriptRoot\driver_bridge.cpp" /Fe:driver-bridge.exe /link d3d12.lib dxgi.lib gdi32.lib ws2_32.lib bcrypt.lib user32.lib dwmapi.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\rgba_oracle.cpp" /Fe:rgba-oracle.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine "$PSScriptRoot\native_stream_probe.cpp" /Fe:native-stream-probe.exe /link d3d12.lib dxgi.lib user32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine "$PSScriptRoot\diagnostic_profile_test.cpp" /Fe:diagnostic-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
diagnostic-profile-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 $profileDefine "$PSScriptRoot\binary_capture_test.cpp" /Fe:binary-capture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
set "WDDM_BRIDGE_CAPTURE_DIRECTORY=$captureTestDirectory"
binary-capture-test.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\context_native_probe.cpp" /Fe:context-native-probe.exe /link dxgi.lib gdi32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\query_native_probe.cpp" /Fe:query-native-probe.exe /link dxgi.lib gdi32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\queue_dependency_probe.cpp" /Fe:queue-dependency-probe.exe /link dxgi.lib gdi32.lib WinHvPlatform.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\vendor_lock_probe.cpp" /Fe:vendor-lock-probe.exe /link dxgi.lib gdi32.lib WinHvPlatform.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 "$PSScriptRoot\sync_sharing_probe.cpp" /Fe:sync-sharing-probe.exe /link d3d12.lib dxgi.lib gdi32.lib
if not "%errorlevel%"=="0" exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /DUNICODE /D_UNICODE "$PSScriptRoot\native_gpu.cpp" /Fe:native-gpu.exe /link d3d12.lib dxgi.lib user32.lib gdi32.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $env:ComSpec /d /c $batch
if ($LASTEXITCODE -ne 0) { throw "Native GPU build failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party/nlohmann-json/LICENSE.MIT') -Destination (Join-Path $output 'nlohmann-json-LICENSE.MIT') -Force
