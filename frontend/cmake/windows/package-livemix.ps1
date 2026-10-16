<#
.SYNOPSIS
    Builds the LiveMix Windows installer from a built rundir.

.DESCRIPTION
    Stages build_vs2022\rundir\<Config> without the portable settings folder and
    debug symbols, adds the Visual C++ runtime next to obs64.exe, and compiles
    livemix-installer.iss with Inno Setup 6. The version comes from
    LIVEMIX_VERSION in frontend\CMakeLists.txt.

    The result, LiveMix-<version>-Setup.exe, is what a GitHub release of
    tuvshinorg/livemix must carry for the in-app updater to install it.

.EXAMPLE
    .\frontend\cmake\windows\package-livemix.ps1 -Config RelWithDebInfo
#>
param(
    [string]$BuildDir = "build_vs2022",
    [string]$Config = "RelWithDebInfo",
    [string]$OutputDir = "release"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repoRoot $BuildDir }
if (-not [System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir = Join-Path $repoRoot $OutputDir }

$cmakeLists = Get-Content (Join-Path $repoRoot "frontend\CMakeLists.txt") -Raw
if ($cmakeLists -notmatch 'set\(LIVEMIX_VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\)') {
    throw "LIVEMIX_VERSION not found in frontend\CMakeLists.txt"
}
$version = $Matches[1]

$rundir = Join-Path $BuildDir "rundir\$Config"
if (-not (Test-Path (Join-Path $rundir "bin\64bit\obs64.exe"))) {
    throw "No build found in $rundir. Build the $Config configuration first."
}

$iscc = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) is not installed." }

# Stage the rundir: no portable settings (they hold the developer's own documents),
# no portable-mode marker and no debug symbols.
$stage = Join-Path $BuildDir "livemix-package\$Config"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

robocopy $rundir $stage /E /NFL /NDL /NJH /NJS /NP `
    /XD (Join-Path $rundir "config") `
    /XF *.pdb portable_mode portable_mode.txt obs_portable_mode obs_portable_mode.txt | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with code $LASTEXITCODE" }

# Ship the Visual C++ runtime next to obs64.exe so LiveMix starts on a clean Windows install.
$vsRoots = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022",
    "$env:ProgramFiles\Microsoft Visual Studio\2022"
) | Where-Object { Test-Path $_ }
$crt = $vsRoots | ForEach-Object {
    Get-ChildItem -Path $_ -Directory -Filter "Microsoft.VC143.CRT" -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\Redist\\MSVC\\[0-9.]+\\x64\\Microsoft\.VC143\.CRT$' }
} | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw "Visual C++ x64 runtime (Microsoft.VC143.CRT) not found." }
Copy-Item (Join-Path $crt.FullName "*.dll") (Join-Path $stage "bin\64bit") -Force

New-Item -ItemType Directory -Force $OutputDir | Out-Null
& $iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DOutputDir=$OutputDir" "/DRepoRoot=$repoRoot" `
    (Join-Path $PSScriptRoot "livemix-installer.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with code $LASTEXITCODE" }

$installer = Join-Path $OutputDir "LiveMix-$version-Setup.exe"
$hash = (Get-FileHash $installer -Algorithm SHA256).Hash.ToLower()
Write-Host "Built $installer"
Write-Host "SHA-256 $hash"
