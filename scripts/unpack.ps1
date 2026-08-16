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
$activeCarrierBackup = Join-Path $gameBin ".AllodsUnpacker14.$PID.pango.dll"
$unpackerDll = Join-Path $gameBin 'AllodsUnpacker14.dll'
$unpackerIni = Join-Path $gameBin 'AllodsUnpacker14.ini'
$trigger = Join-Path $gameBin 'WRITE_NOW'
$log = Join-Path $gameBin 'AllodsUnpacker14.log'
$globalConfig = Join-Path $ClientDir 'Personal\Global.cfg'
$process = $null
$carrierRestoreSource = $null
$globalConfigBytes = $null
$globalConfigChanged = $false

function Invoke-UnpackerBuild {
    $nmake = Get-Command 'nmake.exe' -ErrorAction SilentlyContinue
    if ($nmake -and $env:VSCMD_ARG_TGT_ARCH -eq 'x86') {
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
    if (-not (Test-Path -LiteralPath $stockCarrier)) {
        throw "The active pango.dll was not found at '$stockCarrier'."
    }
    if (Test-Path -LiteralPath $carrierBackup) {
        if (Test-Path -LiteralPath $activeCarrierBackup) {
            throw "Temporary carrier backup already exists at '$activeCarrierBackup'."
        }
        Move-Item -LiteralPath $stockCarrier -Destination $activeCarrierBackup
        $carrierRestoreSource = $activeCarrierBackup
    }
    else {
        Move-Item -LiteralPath $stockCarrier -Destination $carrierBackup
        $carrierRestoreSource = $carrierBackup
    }

    Copy-Item -LiteralPath $builtCarrier -Destination $stockCarrier -Force
    Copy-Item -LiteralPath $builtUnpacker -Destination $unpackerDll -Force
    Copy-Item -LiteralPath (Join-Path $root 'config\AllodsUnpacker14.ini') -Destination $unpackerIni -Force

    # D3D9 exclusive fullscreen device creation fails in Remote Desktop sessions
    # before the pack database is ready. Force windowed startup for this run and
    # restore the user's configuration byte-for-byte in finally.
    if (Test-Path -LiteralPath $globalConfig -PathType Leaf) {
        $globalConfigBytes = [IO.File]::ReadAllBytes($globalConfig)
        $globalConfigText = [Text.Encoding]::UTF8.GetString($globalConfigBytes)
        $windowedText = [Text.RegularExpressions.Regex]::Replace(
            $globalConfigText, '(?m)^gfxFullScreen=1(?=\r?$)', 'gfxFullScreen=0')
        if ($windowedText -ne $globalConfigText) {
            [IO.File]::WriteAllText($globalConfig, $windowedText, [Text.UTF8Encoding]::new($false))
            $globalConfigChanged = $true
            Write-Host 'Temporarily forcing windowed mode for reliable D3D9 startup.'
        }
    }

    # A single 32-bit process accumulates serializer state across the 486k base
    # resources and every map database. Full extraction therefore runs one
    # top-level resource scope per process. Explicit scopes and limited smoke
    # runs remain single-process.
    if ($Scope -or $Limit) {
        [string[]]$runScopes = @($Scope)
    }
    else {
        [string[]]$runScopes = @(
            'Account', 'Characters', 'Client', 'Creatures', 'CutScenes',
            'Interface', 'ItemMall', 'Items', 'Maps', 'Material', 'Mechanics', 'Mods',
            'SFX', 'Ships', 'Spells', 'System', 'World'
        )
    }

    for ($scopeIndex = 0; $scopeIndex -lt $runScopes.Count; ++$scopeIndex) {
        $runScope = $runScopes[$scopeIndex]
        $ini = foreach ($line in Get-Content -LiteralPath $unpackerIni) {
            if ($line -match '^OutputDir=') { "OutputDir=$OutputDir" }
            elseif ($line -match '^Scope=') { "Scope=$runScope" }
            elseif ($line -match '^Limit=') { "Limit=$Limit" }
            else { $line }
        }
        Set-Content -LiteralPath $unpackerIni -Value $ini -Encoding Ascii
        Remove-Item -LiteralPath $trigger -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue

        $label = if ($runScope) { $runScope } else { '<all>' }
        Write-Host "Waiting for client database (scope $label)..."
        $process = Start-Process -FilePath $gameExe -WorkingDirectory $gameBin -WindowStyle Hidden -PassThru
        $deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
        while ([DateTime]::UtcNow -lt $deadline) {
            if ($process.HasExited) { throw "AOgame.exe exited with code $($process.ExitCode)." }
            if ((Test-Path -LiteralPath $log) -and
                (Select-String -LiteralPath $log -SimpleMatch 'main thread frozen' -Quiet)) { break }
            Start-Sleep -Milliseconds 250
            $process.Refresh()
        }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Timed out waiting for the client freeze.' }

        New-Item -ItemType File -Path $trigger -Force | Out-Null
        Write-Host "Extraction started (scope $label)..."
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
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        $process.WaitForExit(5000) | Out-Null
        $process.Dispose()
        $process = $null
        Remove-Item -LiteralPath $trigger -Force -ErrorAction SilentlyContinue
        if ($scopeIndex + 1 -lt $runScopes.Count) { Start-Sleep -Seconds 8 }
    }
    Write-Host "Extraction complete: $OutputDir"
}
finally {
    if ($process) {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        $process.WaitForExit(5000) | Out-Null
        $process.Dispose()
    }
    Remove-Item -LiteralPath $trigger -Force -ErrorAction SilentlyContinue
    if ($globalConfigChanged -and $globalConfigBytes) {
        [IO.File]::WriteAllBytes($globalConfig, $globalConfigBytes)
    }
    if ($carrierRestoreSource -and (Test-Path -LiteralPath $carrierRestoreSource)) {
        $restoreError = $null
        for ($attempt = 0; $attempt -lt 50; ++$attempt) {
            try {
                Remove-Item -LiteralPath $stockCarrier -Force -ErrorAction SilentlyContinue
                Move-Item -LiteralPath $carrierRestoreSource -Destination $stockCarrier -ErrorAction Stop
                $restoreError = $null
                break
            }
            catch {
                $restoreError = $_
                Start-Sleep -Milliseconds 100
            }
        }
        if ($restoreError) { throw "Failed to restore the stock pango.dll: $restoreError" }
    }
}
