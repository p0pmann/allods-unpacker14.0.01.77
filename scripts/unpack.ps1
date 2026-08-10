[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ClientDir,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDir,

    [ValidateScript({ $_ -notmatch '[\r\n]' })]
    [string]$Scope = '',

    [ValidateRange(0, [int]::MaxValue)]
    [int]$Limit = 0,

    [ValidateRange(1, 1440)]
    [int]$TimeoutMinutes = 30,

    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
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

function Invoke-UnpackerBuild {
    $nmake = Get-Command 'nmake.exe' -ErrorAction SilentlyContinue
    if ($nmake) {
        Push-Location $root
        try {
            & $nmake.Source /nologo /f Makefile
            if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
        }
        finally { Pop-Location }
        return
    }

    $vcvars = $null
    $installRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
    foreach ($installRoot in $installRoots) {
        foreach ($version in @('18', '2022', '2019', '2017')) {
            foreach ($edition in @('Community', 'Professional', 'Enterprise', 'BuildTools')) {
                $candidate = Join-Path $installRoot "Microsoft Visual Studio\$version\$edition\VC\Auxiliary\Build\vcvarsall.bat"
                if (Test-Path -LiteralPath $candidate) { $vcvars = $candidate; break }
            }
            if ($vcvars) { break }
        }
        if ($vcvars) { break }
    }
    if (-not $vcvars) { throw 'An MSVC installation with the x86 toolchain was not found.' }

    Push-Location $root
    try {
        $command = 'call "{0}" x64_x86 >nul && nmake /nologo /f Makefile' -f $vcvars
        & $env:ComSpec /d /s /c $command
        if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
    }
    finally { Pop-Location }
}

if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw "AOgame.exe was not found at '$gameExe'."
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

if (-not $NoBuild) {
    Invoke-UnpackerBuild
}

$builtCarrier = Join-Path $root 'build\pango.dll'
$builtUnpacker = Join-Path $root 'build\AllodsUnpacker14.dll'
if (-not (Test-Path -LiteralPath $builtCarrier) -or -not (Test-Path -LiteralPath $builtUnpacker)) {
    throw 'Build artifacts are missing. Run nmake first or omit -NoBuild.'
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

    $ini = foreach ($line in Get-Content -LiteralPath $unpackerIni) {
        if ($line -match '^OutputDir=') { "OutputDir=$OutputDir" }
        elseif ($line -match '^Scope=') { "Scope=$Scope" }
        elseif ($line -match '^Limit=') { "Limit=$Limit" }
        else { $line }
    }
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
            (Select-String -LiteralPath $log -SimpleMatch 'ALL DONE' -Quiet)) { break }
        Start-Sleep -Seconds 1
        $process.Refresh()
    }
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Timed out waiting for extraction to complete.' }

    Select-String -LiteralPath $log -Pattern ' done selected=' | ForEach-Object {
        Write-Host "Extraction summary: $($_.Line.Trim())"
    }
    Write-Host "Extraction complete: $OutputDir"
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
