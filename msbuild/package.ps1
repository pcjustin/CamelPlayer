<#
.SYNOPSIS
  Stages build\<Configuration>\App into dist\CamelPlayer and zips it (docs/Plan.md §4, M0-08).
  Called by build.ps1 -Package.
#>
param([ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$app = Join-Path $root "build\$Configuration\App"
$cli = Join-Path $root "build\$Configuration\camelcli\camelcli.exe"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'CamelPlayer'
if (-not (Test-Path (Join-Path $app 'CamelPlayer.exe'))) { throw "build first: $app\CamelPlayer.exe not found" }
& (Join-Path $PSScriptRoot 'collect-licenses.ps1')

$distPath = [System.IO.Path]::GetFullPath($dist).TrimEnd('\')
$stagePath = [System.IO.Path]::GetFullPath($stage)
if (-not $stagePath.StartsWith($distPath + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Package staging path must remain inside dist.' }
if (Test-Path $stagePath) { Remove-Item -LiteralPath $stagePath -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# The self-contained Windows App SDK unpacks its whole framework, including AI / ML / Widgets components
# CamelPlayer never loads (~45 MB). Leave them and build-only files out.
$exclude = @('*.pdb', '*.lib', '*.exp', 'CamelPlayer.winmd', 'onnxruntime*.dll', 'DirectML.dll',
             'Microsoft.Windows.AI.*', 'Microsoft.Windows.Widgets.*')
Get-ChildItem $app -Recurse -File | Where-Object {
    $name = $_.Name
    -not ($exclude | Where-Object { $name -like $_ })
} | ForEach-Object {
    $rel = $_.FullName.Substring($app.Length + 1)
    $dest = Join-Path $stage $rel
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Copy-Item $_.FullName $dest
}
Copy-Item $cli $stage

# VC++ runtime, app-local (docs/Plan.md §15.1)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'VC++ redist folder not found' }
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item (Join-Path $crt.FullName $dll) $stage }

# Preserve the complete notice collection, including nested crate and runtime notices.
$lic = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\licenses\*') $lic -Recurse -Force
Copy-Item (Join-Path $root 'THIRD-PARTY-NOTICES.md') $stage
Copy-Item (Join-Path $root 'third_party\dependencies.json') (Join-Path $stage 'dependencies.json')

# Registry archives can contain timestamps outside ZIP's supported range.
$zipMinDate = [datetime]'1980-01-02'
$zipMaxDate = [datetime]'2107-12-30'
Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    if ($_.LastWriteTime -lt $zipMinDate) { $_.LastWriteTime = $zipMinDate }
    elseif ($_.LastWriteTime -gt $zipMaxDate) { $_.LastWriteTime = $zipMaxDate }
}

$zip = Join-Path $dist 'CamelPlayer-win-x64.zip'
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

# Symbols go in a separate archive
$pdbZip = Join-Path $dist 'CamelPlayer-win-x64-pdb.zip'
if (Test-Path $pdbZip) { Remove-Item $pdbZip }
Compress-Archive -Path (Join-Path $app 'CamelPlayer.pdb'), ($cli -replace '\.exe$', '.pdb') -DestinationPath $pdbZip -CompressionLevel Optimal

$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "packaged $zip ($mb MB)"
