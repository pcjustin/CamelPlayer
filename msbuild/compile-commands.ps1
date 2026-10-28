<#
.SYNOPSIS
  Writes build\compile_commands.json from the MSBuild projects for clangd and clang-tidy. Called by build.ps1.
#>

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

param(
    [Parameter(Mandatory)] [string] $MSBuild,
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot

$projects = 'src\Common\Common.vcxproj', 'src\OutputBridge\OutputBridge.vcxproj', 'src\Engine\Engine.vcxproj',
            'src\Server\Server.vcxproj', 'src\ControlPoint\App.vcxproj', 'src\Updater\Updater.vcxproj',
            'src\Tools\camelcli\camelcli.vcxproj', 'tests\tests.vcxproj'
$entries = foreach ($project in $projects) {
    $json = & $MSBuild (Join-Path $root $project) -nologo -v:q -t:GetClCommandLines "-p:Configuration=$Configuration" `
        '-p:Platform=x64' "-p:SolutionDir=$root\" -getTargetResult:GetClCommandLines
    if ($LASTEXITCODE) { throw "GetClCommandLines failed for $project" }
    foreach ($item in (($json -join "`n") | ConvertFrom-Json).TargetResults.GetClCommandLines.Items) {
        # clang cannot read MSVC precompiled headers (every source includes its pch.h itself) or #using metadata.
        $options = $item.Identity -replace '/(Y[cu]|Fp|FU)("[^"]*"|\S*)', ''
        foreach ($file in $item.Files -split ';') {
            [ordered]@{ directory = $item.WorkingDirectory; file = $file; command = "clang-cl.exe $options `"$file`"" }
        }
    }
}
$out = Join-Path $root 'build\compile_commands.json'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
ConvertTo-Json @($entries) -Depth 3 | Set-Content -Encoding utf8 $out
