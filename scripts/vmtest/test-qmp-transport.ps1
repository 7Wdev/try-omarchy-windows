param([Parameter(Mandatory)][string]$Qemu)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\qmp-transport.ps1"
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('tom-qmp-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$process = $null
try {
    $env:TEMP = $testRoot
    $env:TMP = $testRoot
    $path = Get-OmarchyQmpPath
    if ($path -ne (Join-Path $testRoot 'TryOmarchyIPC\tools.sock')) { throw 'Fixture escaped its isolated temporary directory' }
    Initialize-OmarchyQmpControl
    $arguments = @('-machine','none','-nodefaults','-display','none','-S','-qmp',("unix:{0},server=on,wait=off" -f $path))
    $process = Start-Process -FilePath $Qemu -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $path)) {
        if ($process.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'QEMU did not create its private socket' }
        Start-Sleep -Milliseconds 50
    }
    # Reuse one PowerShell/.NET process: disposing a shared async wait handle
    # previously broke the second connection even when the first succeeded.
    1..20 | ForEach-Object {
        $stream = New-OmarchyQmpStream
        try {
            $stream.ReadTimeout = 3000
            $reader = [IO.StreamReader]::new($stream)
            $writer = [IO.StreamWriter]::new($stream)
            $writer.AutoFlush = $true
            $greeting = $reader.ReadLine() | ConvertFrom-Json
            if (-not $greeting.QMP) { throw 'Missing QMP greeting' }
            $writer.WriteLine('{"execute":"qmp_capabilities"}')
            $reply = $reader.ReadLine() | ConvertFrom-Json
            if ($reply.error) { throw 'QMP capabilities failed' }
            $writer.WriteLine('{"execute":"query-status"}')
            $reply = $reader.ReadLine() | ConvertFrom-Json
            if ($reply.error -or $reply.return.running -ne $false) { throw 'Unexpected fixture status' }
        } finally { $stream.Dispose() }
    }
    Write-Output 'PASS: 20 consecutive private QMP connections and status queries'
} finally {
    if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    # Remove only the known socket and empty directories created by this fixture.
    $socketPath = Join-Path $testRoot 'TryOmarchyIPC\tools.sock'
    if (Test-Path -LiteralPath $socketPath) { Remove-Item -LiteralPath $socketPath -Force }
    if (Test-Path -LiteralPath (Join-Path $testRoot 'TryOmarchyIPC')) { [IO.Directory]::Delete((Join-Path $testRoot 'TryOmarchyIPC')) }
    # Preserve anything outside the known fixture files.
    if ((Test-Path -LiteralPath $testRoot) -and @(Get-ChildItem -LiteralPath $testRoot -Force).Count -eq 0) { [IO.Directory]::Delete($testRoot) }
}
