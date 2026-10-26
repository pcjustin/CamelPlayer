<#
.SYNOPSIS
  Fails when a tracked file lacks the MPL-2.0 header, a dependency is missing from third_party/dependencies.json,
  its license text or THIRD-PARTY-NOTICES.md, or a file under testdata/ is tracked. Called by build.ps1 and package.ps1.
#>

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$problems = [System.Collections.Generic.List[string]]::new()

$tracked = @(git -C $root ls-files)
if ($LASTEXITCODE) { throw 'git ls-files failed' }
$headerFiles = $tracked | Where-Object {
    ($_ -match '\.(cpp|h|c|idl|xaml|rs|ps1|cmd|rc|toml|props|targets|vcxproj|slnx|manifest|resw|config|cmake)$' -or
     $_ -match '(^|/)(CMakeLists\.txt|\.clang-format|\.editorconfig|\.gitignore)$') -and $_ -notlike 'third_party/licenses/*'
}
foreach ($file in $headerFiles) {
    $head = [System.IO.File]::ReadAllText((Join-Path $root $file))
    $head = $head.Substring(0, [Math]::Min(1500, $head.Length))
    if ($head -notmatch 'Mozilla Public' -or $head -notmatch 'mozilla\.org/MPL/2\.0/') { $problems.Add("missing MPL-2.0 header: $file") }
}
foreach ($file in $tracked | Where-Object { $_ -like 'testdata/*' }) { $problems.Add("tracked test recording: $file") }

$inventory = Get-Content -Raw (Join-Path $root 'third_party\dependencies.json') | ConvertFrom-Json
$notices = Get-Content (Join-Path $root 'THIRD-PARTY-NOTICES.md')
$entries = @($inventory.native) + @($inventory.rust) + @($inventory.microsoft) + @($inventory.build_tools)
foreach ($entry in $entries) {
    $notice = Join-Path $root "third_party\$($entry.notice)"
    $hasText = if (Test-Path $notice -PathType Container) { @(Get-ChildItem $notice -Recurse -File | Where-Object Length).Count }
               else { (Test-Path $notice) -and (Get-Item $notice).Length }
    if (-not $hasText) { $problems.Add("no license text for $($entry.name) at third_party/$($entry.notice)") }
    $listed = $notices | Where-Object { $_.Contains("``$($entry.name)``") -and $_.Contains($entry.version) }
    if (-not $listed) { $problems.Add("THIRD-PARTY-NOTICES.md does not list ``$($entry.name)`` $($entry.version)") }
}

function Find-Entry($list, $name, $version) {
    @($list | Where-Object { $_.name -eq $name -and (-not $version -or $_.version -eq $version) }).Count
}
$manifest = Get-Content -Raw (Join-Path $root 'vcpkg.json') | ConvertFrom-Json
foreach ($dependency in $manifest.dependencies) {
    $name = if ($dependency -is [string]) { $dependency } else { $dependency.name }
    if (-not (Find-Entry $inventory.native $name)) { $problems.Add("vcpkg.json dependency not in the inventory: $name") }
}
$status = Join-Path $root 'vcpkg_installed\vcpkg\status'
if (Test-Path $status) {
    $package = $null
    foreach ($line in Get-Content $status) {
        if ($line -match '^Package: (.+)$') { $package = @{ name = $Matches[1] } }
        elseif ($line -match '^Version: (.+)$') { $package.version = $Matches[1] }
        elseif ($line -match '^Port-Version: (.+)$') { $package.version += "#$($Matches[1])" }
        elseif ($line -match '^Architecture: (.+)$') { $package.triplet = $Matches[1] }
        elseif ($line -match '^Status: .* installed$' -and $package) {
            $list = if ($package.triplet -eq 'x64-windows') { $inventory.build_tools } else { $inventory.native }
            if (-not (Find-Entry $list $package.name $package.version)) {
                $problems.Add("installed vcpkg package not in the inventory: $($package.name) $($package.version) ($($package.triplet))")
            }
        }
    }
}
$crate = $null
foreach ($line in Get-Content (Join-Path $root 'rust\Cargo.lock')) {
    if ($line -eq '[[package]]') { $crate = @{} }
    elseif ($line -match '^name = "(.+)"$') { $crate.name = $Matches[1] }
    elseif ($line -match '^version = "(.+)"$') { $crate.version = $Matches[1] }
    elseif ($line -match '^source = ' -and -not (Find-Entry $inventory.rust $crate.name $crate.version)) {
        $problems.Add("Cargo.lock crate not in the inventory: $($crate.name) $($crate.version)")
    }
}
[xml]$packages = Get-Content (Join-Path $root 'src\ControlPoint\packages.config')
foreach ($package in $packages.packages.package) {
    if (-not (Find-Entry $inventory.microsoft $package.id $package.version)) {
        $problems.Add("NuGet package not in the inventory: $($package.id) $($package.version)")
    }
}

if ($problems.Count) {
    $problems | ForEach-Object { Write-Host $_ }
    throw "License check failed with $($problems.Count) problem(s)"
}
Write-Host "License check passed: $($headerFiles.Count) headers, $($entries.Count) dependencies."
