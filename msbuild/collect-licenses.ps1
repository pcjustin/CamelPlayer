$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$destination = Join-Path $root 'third_party\licenses'
New-Item -ItemType Directory -Force $destination | Out-Null
$inventory = [ordered]@{ project = 'CamelPlayer v2'; native = @(); rust = @(); microsoft = @() }

$nativeLicenses = [ordered]@{
    ffmpeg = 'LGPL-2.1-or-later'; libsndfile = 'LGPL-2.1-or-later'; drlibs = 'Unlicense OR MIT-0'
    libflac = 'BSD-3-Clause'; libogg = 'BSD-3-Clause'; libvorbis = 'BSD-3-Clause'; opus = 'BSD-3-Clause'
    zlib = 'Zlib'; wil = 'MIT'; 'nlohmann-json' = 'MIT'; gtest = 'BSD-3-Clause'
    mpg123 = 'LGPL-2.1-or-later'; mp3lame = 'LGPL-2.0-only'; soxr = 'LGPL-2.1-or-later'
}
$testOnly = @('gtest', 'mp3lame') # linked into tests.exe only, never shipped
foreach ($port in $nativeLicenses.Keys) {
    $share = Join-Path $root "vcpkg_installed\x64-windows-static-md\share\$port"
    $notice = Join-Path $share 'copyright'
    $spdx = Get-Content -Raw (Join-Path $share 'vcpkg.spdx.json') | ConvertFrom-Json
    if (-not (Test-Path $notice)) { throw "Missing license: $notice" }
    $out = Join-Path $destination 'native'
    New-Item -ItemType Directory -Force $out | Out-Null
    Copy-Item -LiteralPath $notice -Destination (Join-Path $out "$port.txt")
    $sources = @($spdx.packages | Where-Object { $_.checksums } | ForEach-Object {
        [ordered]@{ location = $_.downloadLocation; checksums = $_.checksums }
    })
    $inventory.native += [ordered]@{
        name = $port; version = $spdx.packages[0].versionInfo; license = $nativeLicenses[$port]
        role = $(if ($port -in $testOnly) { 'test' } else { 'runtime or verification' })
        port_source = $spdx.packages[0].downloadLocation; sources = $sources
        notice = "licenses/native/$port.txt"
    }
}

$cargo = Join-Path $env:USERPROFILE '.cargo\bin\cargo.exe'
$metadataText = & $cargo metadata --offline --locked --format-version 1 --manifest-path (Join-Path $root 'rust\symphonia_ffi\Cargo.toml')
if ($LASTEXITCODE) { throw 'Cargo metadata failed; restore the locked dependencies first.' }
$metadata = $metadataText | ConvertFrom-Json
foreach ($crate in $metadata.packages | Where-Object { $_.source }) {
    $crateRoot = Split-Path $crate.manifest_path
    $crateName = "$($crate.name)-$($crate.version)"
    $out = Join-Path $destination "rust\$crateName"
    New-Item -ItemType Directory -Force $out | Out-Null
    $notices = @(Get-ChildItem -LiteralPath $crateRoot -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|COPYRIGHT|NOTICE)' })
    foreach ($notice in $notices) { Copy-Item -LiteralPath $notice.FullName -Destination $out }
    if ($crate.license -eq 'MPL-2.0') {
        Copy-Item -LiteralPath (Join-Path $destination 'MPL-2.0.txt') -Destination (Join-Path $out 'LICENSE-MPL-2.0.txt')
    } elseif (-not $notices.Count) { throw "Missing license files for $crateName" }
    $inventory.rust += [ordered]@{
        name = $crate.name; version = $crate.version; license = $crate.license
        source_archive = "https://static.crates.io/crates/$($crate.name)/$crateName.crate"
        notice = "licenses/rust/$crateName/"
    }
}
$rustc = Join-Path $env:USERPROFILE '.cargo\bin\rustc.exe'
$sysroot = & $rustc --print sysroot
if ($LASTEXITCODE) { throw 'Cannot locate Rust runtime notices.' }
$rustNotices = Join-Path $sysroot 'share\doc\rust\COPYRIGHT-library.html'
if (-not (Test-Path $rustNotices)) { throw "Missing Rust runtime notices: $rustNotices" }
$rustOut = Join-Path $destination 'rust-runtime'
New-Item -ItemType Directory -Force $rustOut | Out-Null
Copy-Item -LiteralPath $rustNotices -Destination $rustOut
& $rustc --version | Set-Content -LiteralPath (Join-Path $rustOut 'toolchain.txt') -Encoding utf8

[xml]$packages = Get-Content (Join-Path $root 'src\ControlPoint\packages.config')
foreach ($package in $packages.packages.package) {
    $name = "$($package.id).$($package.version)"
    $packageRoot = Join-Path $root "packages\$name"
    $out = Join-Path $destination "microsoft\$name"
    $files = @(Get-ChildItem -LiteralPath $packageRoot -File | Where-Object { $_.Name -match '^(license|sdk_license|notice)(\.|$)' })
    if ($files.Count) {
        New-Item -ItemType Directory -Force $out | Out-Null
        foreach ($file in $files) { Copy-Item -LiteralPath $file.FullName -Destination $out }
    } elseif ($package.id -ne 'Microsoft.Windows.SDK.BuildTools') { throw "Missing package terms: $name" }
    $inventory.microsoft += [ordered]@{
        name = $package.id; version = $package.version
        source = "https://www.nuget.org/packages/$($package.id)/$($package.version)"
        notice = $(if ($files.Count) { "licenses/microsoft/$name/" } else { 'https://aka.ms/WinSDKLicenseURL' })
        role = $(if ($package.id -match 'BuildTools') { 'build' } else { 'headers or runtime' })
    }
}
$inventory | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $root 'third_party\dependencies.json') -Encoding utf8
Write-Host "Collected notices for $($inventory.native.Count) native packages, $($inventory.rust.Count) Rust crates and $($inventory.microsoft.Count) Microsoft packages."
