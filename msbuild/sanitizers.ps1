<#
.SYNOPSIS
  Runs the checks every release needs on its commit (docs/Plan.md §12.1), each in a build of its own. Called by
  build.ps1.
#>

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

param(
    [Parameter(Mandatory)] [string] $MSBuild,
    [ValidateSet('Asan', 'Ubsan', 'All')] [string] $Run = 'All'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$logs = Join-Path $root 'build\logs'
$rust = Join-Path $root 'rust\Cargo.toml'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -property installationPath

$fuzzSeconds = 600

# Time limits that an instrumented build does not meet; the ordinary Release suite holds them.
$asanExceptions = 'LibraryBrowse.WarmFirstPageMeetsOneHundredMillisecondTargetAtOneHundredThousandTracks',
                  'QueuePolicyTest.AWholeLibraryQueueReordersRestoresAndAdvancesWithoutQuadraticWork'

$lockAnnotated = 'src\Engine\PlaybackEngine.cpp', 'src\Server\Playback\LocalServer.cpp',
                 'src\Server\Network\NetworkOutput.cpp', 'src\Server\Network\AirPlayOutput.cpp'

function Invoke-Build([string] $Project, [string[]] $Properties) {
    $log = Join-Path $logs "sanitizers-$($Properties[-1] -replace '\W', '').log"
    & $MSBuild (Join-Path $root $Project) -nologo -v:m -m:1 '-p:CL_MPCount=1' '-p:Configuration=Release' `
        '-p:Platform=x64' $Properties "-flp:logfile=$log;verbosity=normal"
    if ($LASTEXITCODE) { throw "$Project did not build, see $log" }
}

function Copy-AsanRuntime([string] $To) {
    $runtime = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll') |
        Select-Object -Last 1
    Copy-Item $runtime.FullName (Join-Path $root $To) -Force
}

function Invoke-Logged([string] $Name, [string] $Program, [string[]] $Arguments, [string] $Directory = $root) {
    $log = Join-Path $logs "$Name.log"
    $process = Start-Process $Program -ArgumentList $Arguments -WorkingDirectory $Directory -NoNewWindow -Wait -PassThru `
        -RedirectStandardError $log
    if ($process.ExitCode) { throw "$Name failed ($($process.ExitCode)), see $log" }
}

function Test-Asan {
    Invoke-Build 'tests\tests.vcxproj' '-p:CamelAsan=true'
    Copy-AsanRuntime 'build\Asan\tests'
    & (Join-Path $root 'build\Asan\tests\tests.exe') --gtest_brief=1 "--gtest_filter=-$($asanExceptions -join ':')"
    if ($LASTEXITCODE) { throw "the suite under ASan failed ($LASTEXITCODE)" }
}

function Test-Ubsan {
    Invoke-Build 'tests\tests.vcxproj' '-p:CamelUbsan=true'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    & (Join-Path $root 'build\Ubsan\tests\tests.exe') --gtest_brief=1
    if ($LASTEXITCODE) { throw "the suite under UBSan failed ($LASTEXITCODE)" }
}

function Test-Locks {
    & (Join-Path $PSScriptRoot 'compile-commands.ps1') -MSBuild $MSBuild
    $commands = Get-Content (Join-Path $root 'build\compile_commands.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $rules = Join-Path $vs 'Team Tools\Static Analysis Tools\Rule Sets\ConcurrencyCheckRules.ruleset'
    $work = Join-Path $root 'build\obj\Analyze'
    New-Item -ItemType Directory -Force $work | Out-Null
    $findings = foreach ($file in $lockAnnotated) {
        $path = Join-Path $root $file
        $entry = $commands | Where-Object { $_.file -eq $path } | Select-Object -First 1
        if (-not $entry) { throw "$file is not in the compile database" }
        $options = $entry.command.Replace("`"$path`"", '') -replace '^clang-cl\.exe ', '' `
            -replace '(?i)\s/(Fo|Fd)("[^"]*"|\S*)', '' -replace '\s/(c|FS|MP\d*|ZI|Zi)(?=\s|$)', ''
        Set-Content (Join-Path $work 'options.rsp') $options -Encoding ASCII
        Set-Content (Join-Path $work 'analyze.cmd') @"
@call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
@cd /d "$($entry.directory)"
@cl /nologo @"$work\options.rsp" /analyze /analyze:plugin EspXEngine.dll /analyze:ruleset "$rules" /c "$path" /Fo"$work\out.obj" 2>&1
"@ -Encoding ASCII
        $output = & cmd.exe /c (Join-Path $work 'analyze.cmd')
        if ($LASTEXITCODE) { "$file did not compile: $(($output | Select-String 'error') -join ' ')" }
        $output | Select-String 'warning C26\d+' | ForEach-Object { "$_" } | Sort-Object -Unique
    }
    if ($findings) { $findings | Write-Host; throw "/analyze reported $(@($findings).Count) findings" }
}

function Test-Loom {
    Invoke-Logged 'loom' 'cargo' @('test', '--release', '--locked', '--manifest-path',
        "`"$(Join-Path $root 'rust\ring_loom\Cargo.toml')`"")
}

function Test-Miri {
    foreach ($module in 'ring', 'dts_abi', 'dsd_pcm_abi') {
        Invoke-Logged "miri-$module" 'cargo' @('+nightly', 'miri', 'test', '--locked', '--manifest-path', "`"$rust`"",
            '-p', 'camel_audio_engine', "${module}::")
    }
}

function Test-CargoFuzz {
    Invoke-Logged 'fuzz-frames' 'cargo' @('+nightly', 'fuzz', 'run', 'frames', '--sanitizer', 'none', '--target-dir',
        '../../build/cargo-fuzz', 'fuzz/corpus/frames', '../../tests/fixtures', '--', "-max_total_time=$fuzzSeconds",
        '-timeout=10', '-rss_limit_mb=1500', '-max_len=65536') (Join-Path $root 'rust\dts_core')
}

function Test-CppFuzz([string] $Target, [string] $Seeds) {
    Invoke-Build 'src\Tools\fuzz\fuzz.vcxproj' @('-p:CamelFuzz=true', "-p:CamelFuzzTarget=$Target")
    Copy-AsanRuntime 'build\Fuzz\fuzz'
    $corpus = Join-Path $root "build\Fuzz\corpus\$Target"
    New-Item -ItemType Directory -Force $corpus | Out-Null
    Invoke-Logged "fuzz-$Target" (Join-Path $root "build\Fuzz\fuzz\$Target.exe") @("`"$corpus`"", "`"$Seeds`"",
        "-max_total_time=$fuzzSeconds", '-timeout=10', '-rss_limit_mb=2048', '-max_len=131072',
        "`"-artifact_prefix=$root\build\Fuzz\$Target-`"")
}

$checks = [ordered]@{}
if ($Run -eq 'All') {
    $checks['/analyze on the annotated locks'] = { Test-Locks }
    $checks['Loom on the ring'] = { Test-Loom }
    $checks['Miri on unsafe code'] = { Test-Miri }
}
if ($Run -ne 'Ubsan') { $checks['Suite under ASan'] = { Test-Asan } }
if ($Run -ne 'Asan') { $checks['Suite under UBSan'] = { Test-Ubsan } }
if ($Run -eq 'All') {
    $checks['Fuzz the DTS frame reader'] = { Test-CargoFuzz }
    $checks['Fuzz the format probe'] = { Test-CppFuzz 'probe' (Join-Path $root 'tests\fixtures') }
    $checks['Fuzz the network messages'] = { Test-CppFuzz 'network' (Join-Path $root 'src\Tools\fuzz\seeds\network') }
    $checks['Fuzz the zip reader'] = { Test-CppFuzz 'zip' (Join-Path $root 'src\Tools\fuzz\seeds\zip') }
}

New-Item -ItemType Directory -Force $logs | Out-Null
$failures = @()
foreach ($check in $checks.GetEnumerator()) {
    Write-Host "== $($check.Key)"
    $started = Get-Date
    try {
        & $check.Value | Out-Host
        Write-Host "== passed in $([int]((Get-Date) - $started).TotalSeconds) s"
    } catch {
        Write-Host "== FAILED: $_"
        $failures += "$($check.Key): $_"
    }
}
if ($failures) {
    Write-Host "`nFailed:"
    $failures | Write-Host
    exit 1
}
Write-Host "`nAll $($checks.Count) checks passed."
