# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$destination = Join-Path $root 'third_party\licenses'
New-Item -ItemType Directory -Force $destination | Out-Null
$inventory = [ordered]@{ project = 'CamelPlayer'; native = @(); rust = @(); microsoft = @(); build_tools = @() }

$nativeLicenses = [ordered]@{
    ffmpeg = 'LGPL-2.1-or-later'; libsndfile = 'LGPL-2.1-or-later'; drlibs = 'Unlicense OR MIT-0'
    libflac = 'BSD-3-Clause'; libogg = 'BSD-3-Clause'; libvorbis = 'BSD-3-Clause'; opus = 'BSD-3-Clause'
    zlib = 'Zlib'; wil = 'MIT'; 'nlohmann-json' = 'MIT'; gtest = 'BSD-3-Clause'
    mpg123 = 'LGPL-2.1-or-later'; mp3lame = 'LGPL-2.0-only'; soxr = 'LGPL-2.1-or-later'
    'dsd-nexus' = 'LGPL-2.1-or-later'; sqlite3 = 'blessing'; taglib = 'LGPL-2.1-only OR MPL-1.1'; utfcpp = 'BSL-1.0'
    libwebp = 'BSD-3-Clause'; opencc = 'Apache-2.0'; 'marisa-trie' = 'BSD-2-Clause'; 'darts-clone' = 'BSD-2-Clause'; rapidjson = 'MIT'
    tclap = 'MIT'
}
$testOnly = @('gtest', 'mp3lame') # linked into tests.exe only, never shipped
$buildOnly = @('tclap') # OpenCC's command-line tools only; not part of the OpenCC library
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
        role = $(if ($port -in $testOnly) { 'test' } elseif ($port -in $buildOnly) { 'build' } else { 'runtime or verification' })
        port_source = $spdx.packages[0].downloadLocation; sources = $sources
        notice = "licenses/native/$port.txt"
    }
}

# vcpkg host tools run while building the libraries above; none is linked or shipped.
$hostLicenses = [ordered]@{
    'ffmpeg-bin2c' = 'MIT'; pkgconf = 'ISC AND BSD-2-Clause AND BSD-4-Clause (getopt) AND GPL-2.0-or-later WITH Autoconf-exception-generic (pkg.m4)'
    'vcpkg-cmake' = 'MIT'; 'vcpkg-cmake-config' = 'MIT'; 'vcpkg-cmake-get-vars' = 'MIT'; 'vcpkg-msbuild' = 'MIT'
    'vcpkg-pkgconfig-get-modules' = 'MIT'; 'vcpkg-tool-meson' = 'MIT (port scripts) AND Apache-2.0 (Meson)'
    yasm = 'BSD-2-Clause AND BSD-3-Clause AND (Artistic-1.0 OR GPL-2.0-only OR LGPL-2.0-only) (bitvect)'
}
foreach ($port in $hostLicenses.Keys) {
    $share = Join-Path $root "vcpkg_installed\x64-windows\share\$port"
    $notice = Join-Path $share 'copyright'
    if (-not (Test-Path $notice)) { throw "Missing license: $notice" }
    $spdx = Get-Content -Raw (Join-Path $share 'vcpkg.spdx.json') | ConvertFrom-Json
    $out = Join-Path $destination "build-tools\$port"
    New-Item -ItemType Directory -Force $out | Out-Null
    Copy-Item -LiteralPath $notice -Destination (Join-Path $out 'copyright.txt')
    if ($port -eq 'vcpkg-tool-meson') {
        $meson = Join-Path $env:LOCALAPPDATA "vcpkg\downloads\meson-$($spdx.packages[0].versionInfo -replace '#.*').tar.gz"
        $mesonName = [System.IO.Path]::GetFileNameWithoutExtension([System.IO.Path]::GetFileNameWithoutExtension($meson))
        $copying = & "$env:SystemRoot\System32\tar.exe" -xOzf $meson "$mesonName/COPYING"
        if ($LASTEXITCODE) { throw "Missing Meson license in $meson" }
        $copying | Set-Content -LiteralPath (Join-Path $out 'meson-COPYING.txt') -Encoding utf8
    }
    $inventory.build_tools += [ordered]@{
        name = $port; version = $spdx.packages[0].versionInfo; license = $hostLicenses[$port]; role = 'build'
        port_source = $spdx.packages[0].downloadLocation; notice = "licenses/build-tools/$port/"
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
$rustVersion = & $rustc --version
$rustVersion | Set-Content -LiteralPath (Join-Path $rustOut 'toolchain.txt') -Encoding utf8
$inventory.rust += [ordered]@{
    name = 'Rust standard library'; version = ($rustVersion -split ' ')[1]; license = 'MIT OR Apache-2.0'
    source_archive = "https://static.rust-lang.org/dist/rustc-$(($rustVersion -split ' ')[1])-src.tar.xz"
    notice = 'licenses/rust-runtime/'
}

[xml]$packages = Get-Content (Join-Path $root 'src\ControlPoint\packages.config')
foreach ($package in $packages.packages.package) {
    $name = "$($package.id).$($package.version)"
    $packageRoot = Join-Path $root "packages\$name"
    $out = Join-Path $destination "microsoft\$name"
    $files = @(Get-ChildItem -LiteralPath $packageRoot -File | Where-Object { $_.Name -match '^(license|sdk_license|notice)(\.|$)' })
    if ($package.id -eq 'Microsoft.Windows.SDK.BuildTools') {
        # The package carries no terms; the installed Windows SDK of the same release has them.
        $sdkRelease = ($package.version -split '\.')[0..2] -join '.'
        $files = @(Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Licenses\$sdkRelease.0") -File -Filter '*.rtf')
    }
    if (-not $files.Count) { throw "Missing package terms: $name" }
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach ($file in $files) { Copy-Item -LiteralPath $file.FullName -Destination $out }
    $inventory.microsoft += [ordered]@{
        name = $package.id; version = $package.version
        source = "https://www.nuget.org/packages/$($package.id)/$($package.version)"
        notice = "licenses/microsoft/$name/"
        role = $(if ($package.id -match 'BuildTools') { 'build' } else { 'headers or runtime' })
    }
}

# package.ps1 ships the VC++ runtime DLLs app-local; their terms are kept under licenses/microsoft/MSVC-2026.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'VC++ redist folder not found' }
$inventory.microsoft += [ordered]@{
    name = 'Microsoft.VCRedist (msvcp140, vcruntime140, vcruntime140_1)'
    version = (Get-Item (Join-Path $crt.FullName 'msvcp140.dll')).VersionInfo.ProductVersion
    source = 'Visual Studio 2026 (VC\Redist\MSVC)'; notice = 'licenses/microsoft/MSVC-2026/'; role = 'runtime'
}
$inventory | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $root 'third_party\dependencies.json') -Encoding utf8
Write-Host "Collected notices for $($inventory.native.Count) native packages, $($inventory.rust.Count) Rust crates and $($inventory.microsoft.Count) Microsoft packages."
