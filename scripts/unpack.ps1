[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ClientDir,

    [ValidateRange(0, [int]::MaxValue)]
    [int]$Limit = 0,

    [ValidateRange(1, 1440)]
    [int]$TimeoutMinutes = 30,

    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gameBin = Join-Path $ClientDir 'bin'
$gameExe = Join-Path $gameBin 'AOgame.exe'
$stockCarrier = Join-Path $gameBin 'pango.dll'
$carrierBackup = Join-Path $gameBin 'pango_orig.dll'
$unpackerDll = Join-Path $gameBin 'AllodsUnpacker14.dll'
$unpackerIni = Join-Path $gameBin 'AllodsUnpacker14.ini'
$trigger = Join-Path $gameBin 'WRITE_NOW'
$log = Join-Path $gameBin 'AllodsUnpacker14.log'
$process = $null
$installedCarrier = $false

if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw "AOgame.exe was not found at '$gameExe'."
}

if (-not $NoBuild) {
    & (Join-Path $root 'build.cmd')
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
}

$builtCarrier = Join-Path $root 'build\pango.dll'
$builtUnpacker = Join-Path $root 'build\AllodsUnpacker14.dll'
if (-not (Test-Path -LiteralPath $builtCarrier) -or -not (Test-Path -LiteralPath $builtUnpacker)) {
    throw 'Build artifacts are missing. Run build.cmd first or omit -NoBuild.'
}

try {
    if (-not (Test-Path -LiteralPath $carrierBackup)) {
        if (-not (Test-Path -LiteralPath $stockCarrier)) {
            throw "The stock pango.dll was not found at '$stockCarrier'."
        }
        Move-Item -LiteralPath $stockCarrier -Destination $carrierBackup
    }

    Copy-Item -LiteralPath $builtCarrier -Destination $stockCarrier -Force
    $installedCarrier = $true
    Copy-Item -LiteralPath $builtUnpacker -Destination $unpackerDll -Force
    Copy-Item -LiteralPath (Join-Path $root 'config\AllodsUnpacker14.ini') -Destination $unpackerIni -Force

    $ini = Get-Content -LiteralPath $unpackerIni -Raw
    $ini = $ini -replace '(?m)^Limit=.*$', "Limit=$Limit"
    Set-Content -LiteralPath $unpackerIni -Value $ini -Encoding Ascii

    Remove-Item -LiteralPath $trigger -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue

    $process = Start-Process -FilePath $gameExe -WorkingDirectory $gameBin -PassThru
    $deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)

    Write-Host 'Waiting for the client database to become ready...'
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) { throw "AOgame.exe exited with code $($process.ExitCode)." }
        if ((Test-Path -LiteralPath $log) -and
            (Select-String -LiteralPath $log -SimpleMatch 'main thread frozen' -Quiet)) { break }
        Start-Sleep -Milliseconds 250
        $process.Refresh()
    }
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Timed out waiting for the client freeze.' }

    New-Item -ItemType File -Path $trigger -Force | Out-Null
    Write-Host 'Extraction started...'

    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) { throw "AOgame.exe exited with code $($process.ExitCode)." }
        if ((Test-Path -LiteralPath $log) -and
            (Select-String -LiteralPath $log -SimpleMatch 'ALL DONE' -Quiet)) {
            Write-Host "Extraction complete: $(Join-Path $gameBin 'data')"
            return
        }
        Start-Sleep -Seconds 1
        $process.Refresh()
    }
    throw 'Timed out waiting for extraction to complete.'
}
finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(5000) | Out-Null
    }
    Remove-Item -LiteralPath $trigger -Force -ErrorAction SilentlyContinue
    if ($installedCarrier -and (Test-Path -LiteralPath $carrierBackup)) {
        Remove-Item -LiteralPath $stockCarrier -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $carrierBackup -Destination $stockCarrier
    }
}
