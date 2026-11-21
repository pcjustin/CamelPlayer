# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# The newest Visual Studio, prereleases included, and what the build scripts take from it. Dot-sourced by them.

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

function Get-VisualStudio {
    & $vswhere -latest -prerelease -property installationPath
}

function Get-MSBuild {
    & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' |
        Select-Object -First 1
}

# The x64 C and C++ runtime folder of the newest Redist version, ordered by number: as text, 14.9 would follow 14.10.
function Get-VCRuntime {
    $crt = Get-ChildItem (Join-Path (Get-VisualStudio) 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory |
        Sort-Object { $v = [version]'0.0'; [void][version]::TryParse($_.Parent.Parent.Name, [ref]$v); $v }, Name |
        Select-Object -Last 1
    if (-not $crt) { throw 'VC++ redist folder not found' }
    $crt
}
