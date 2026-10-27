<#
.SYNOPSIS
  Builds CamelPlayer, optionally runs the tests and, for Release, packages dist\CamelPlayer-<VERSION>-win-x64.zip
  (docs/Plan.md §4, M0-08).
.EXAMPLE
  .\build.ps1 -Configuration Release -Test
#>

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release',
    [switch] $Test,
    [switch] $NoPackage
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# Storage Sense can wipe %TEMP% during long builds (docs/Plan.md §15), so keep an eye on free space.
$buildDrive = [System.IO.Path]::GetPathRoot($root)
$driveInfo = [System.IO.DriveInfo]::new($buildDrive)
$freeBytes = $driveInfo.AvailableFreeSpace
if ($freeBytes -lt 10GB) {
    $freeGB = ($freeBytes / 1GB).ToString('0.0', [System.Globalization.CultureInfo]::InvariantCulture)
    Write-Warning "Only $freeGB GB free on $buildDrive"
}

# cargo is installed per user by rustup
$cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
if ((Test-Path $cargoBin) -and -not ($env:Path -split ';' -contains $cargoBin)) { $env:Path = "$cargoBin;$env:Path" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (Visual Studio 2026 with C++ desktop workload required)' }

New-Item -ItemType Directory -Force (Join-Path $root 'build\logs') | Out-Null
$log = Join-Path $root "build\logs\build-$Configuration.log"
& $msbuild (Join-Path $root 'CamelPlayer.slnx') -restore '-p:RestorePackagesConfig=true' -m -nologo -v:m `
    "-p:Configuration=$Configuration" '-p:Platform=x64' "-flp:logfile=$log;verbosity=normal"
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE), see $log" }
& (Join-Path $root 'msbuild\check-licenses.ps1')

if ($Test) {
    cargo test --locked --manifest-path (Join-Path $root 'rust\Cargo.toml') -p camel_audio_engine
    if ($LASTEXITCODE) { throw "Rust Engine tests failed ($LASTEXITCODE)" }
    & (Join-Path $root "build\$Configuration\tests\tests.exe") --gtest_brief=1
    if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
}

if ($Configuration -eq 'Release' -and -not $NoPackage) {
    & (Join-Path $root 'msbuild\package.ps1') -Configuration $Configuration
    if ($LASTEXITCODE) { throw "packaging failed ($LASTEXITCODE)" }
}
