<#
.SYNOPSIS
  Builds CamelPlayer, optionally runs the tests and packages dist\CamelPlayer-win-x64.zip (docs/計劃書.md §4, M0-08).
.EXAMPLE
  .\build.ps1 -Configuration Release -Test -Package
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release',
    [switch] $Test,
    [switch] $Package
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# Storage Sense can wipe %TEMP% during long builds (docs/計劃書.md §15), so keep an eye on free space.
$freeGB = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
if ($freeGB -lt 10) { Write-Warning "Only $freeGB GB free on C:" }

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

if ($Test) {
    & (Join-Path $root "build\$Configuration\tests\tests.exe") --gtest_brief=1
    if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
}

if ($Package) {
    & (Join-Path $root 'msbuild\package.ps1') -Configuration $Configuration
    if ($LASTEXITCODE) { throw "packaging failed ($LASTEXITCODE)" }
}
