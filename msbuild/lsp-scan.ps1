<#
.SYNOPSIS
  LSP scan: opens C++ files in clangd as a language server, as VS Code does, and prints the diagnostics it
  publishes (compiler, clang-tidy from .clang-tidy, Include Cleaner from .clangd). Exits 1 if there are any.
  Without -Files it scans the C++ files changed against HEAD, staged, unstaged and untracked.
  clangd is taken from -Clangd, $env:CLANGD, VS Code's clangd.path setting or the extension's install folder.
  Needs build\compile_commands.json from build.cmd.
#>

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

param(
    [string[]] $Files,
    [string] $Clangd,
    [int] $TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot

function Find-Clangd {
    if ($Clangd) { return $Clangd }
    if ($env:CLANGD) { return $env:CLANGD }
    $settings = Join-Path $env:APPDATA 'Code\User\settings.json'
    if (Test-Path $settings) {
        $match = Select-String -Path $settings -Pattern '"clangd\.path"\s*:\s*"([^"]+)"' | Select-Object -First 1
        if ($match) { return $match.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    $installed = Get-ChildItem (Join-Path $env:APPDATA 'Code\User\globalStorage\llvm-vs-code-extensions.vscode-clangd\install') `
        -Recurse -Filter clangd.exe -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($installed) { return $installed.FullName }
    throw 'clangd not found: install the VS Code clangd extension or pass -Clangd'
}

if (-not $Files) {
    $changed = @(git -C $root diff --name-only HEAD) + @(git -C $root ls-files --others --exclude-standard)
    $Files = $changed | Where-Object { $_ -match '\.(cpp|h|hpp|c)$' -and $_ -notmatch 'Generated Files' } | Sort-Object -Unique
}
$paths = @($Files -split ',' | ForEach-Object { (Resolve-Path (Join-Path $root $_) -ErrorAction SilentlyContinue).Path } | Where-Object { $_ })
if (-not $paths) { Write-Host 'LSP scan: no C++ files to scan.'; exit 0 }
if (-not (Test-Path (Join-Path $root 'build\compile_commands.json'))) { throw 'build\compile_commands.json is missing; run build.cmd first' }

# Windows PowerShell gives the child's stdin the console encoding and writes its BOM first, which breaks the
# first LSP header.
[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)
$start = [System.Diagnostics.ProcessStartInfo]::new((Find-Clangd))
$start.Arguments = "--compile-commands-dir=`"$(Join-Path $root 'build')`" --clang-tidy --background-index=false --log=error"
$start.WorkingDirectory = $root
$start.UseShellExecute = $false
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$clangdProcess = [System.Diagnostics.Process]::Start($start)
$clangdProcess.BeginErrorReadLine()
$in = $clangdProcess.StandardInput.BaseStream
$out = $clangdProcess.StandardOutput.BaseStream
$utf8 = [System.Text.UTF8Encoding]::new($false)
$script:nextId = 1

function Send($message) {
    $body = $utf8.GetBytes(($message | ConvertTo-Json -Depth 20 -Compress))
    $header = [System.Text.Encoding]::ASCII.GetBytes("Content-Length: $($body.Length)`r`n`r`n")
    $in.Write($header, 0, $header.Length)
    $in.Write($body, 0, $body.Length)
    $in.Flush()
}

function Read-Exactly([int] $count, [datetime] $deadline) {
    $buffer = [byte[]]::new($count)
    $read = 0
    while ($read -lt $count) {
        $task = $out.ReadAsync($buffer, $read, $count - $read)
        $left = [int][Math]::Max(0, ($deadline - [datetime]::Now).TotalMilliseconds)
        if (-not $task.Wait($left)) { throw "LSP scan: clangd did not answer within $TimeoutSeconds s" }
        if ($task.Result -eq 0) { throw 'LSP scan: clangd exited' }
        $read += $task.Result
    }
    return , $buffer
}

function Receive([datetime] $deadline) {
    $header = ''
    while (-not $header.EndsWith("`r`n`r`n")) { $header += [char](Read-Exactly 1 $deadline)[0] }
    $length = [int]([regex]::Match($header, 'Content-Length:\s*(\d+)').Groups[1].Value)
    $utf8.GetString((Read-Exactly $length $deadline)) | ConvertFrom-Json
}

function Uri([string] $path) { ([System.Uri]::new($path)).AbsoluteUri }
function Key([string] $uri) { [System.Uri]::UnescapeDataString($uri).ToLowerInvariant() }

$deadline = [datetime]::Now.AddSeconds($TimeoutSeconds)
Send @{ jsonrpc = '2.0'; id = $script:nextId++; method = 'initialize'; params = @{
        processId = $PID; rootUri = (Uri $root); capabilities = @{ textDocument = @{ publishDiagnostics = @{} } } } }
do { $reply = Receive $deadline } until ($reply.PSObject.Properties['id'] -and $reply.id -eq 1)
Send @{ jsonrpc = '2.0'; method = 'initialized'; params = @{} }

$pending = @{}
foreach ($path in $paths) {
    $uri = Uri $path
    $pending[(Key $uri)] = $path
    Send @{ jsonrpc = '2.0'; method = 'textDocument/didOpen'; params = @{ textDocument = @{
                uri = $uri; languageId = 'cpp'; version = 1; text = [System.IO.File]::ReadAllText($path) } } }
}

$problems = 0
while ($pending.Count) {
    $message = Receive $deadline
    if (-not $message.PSObject.Properties['method'] -or $message.method -ne 'textDocument/publishDiagnostics') {
        if ($message.PSObject.Properties['id'] -and $message.PSObject.Properties['method']) {
            Send @{ jsonrpc = '2.0'; id = $message.id; result = $null }
        }
        continue
    }
    $key = Key $message.params.uri
    if (-not $pending.ContainsKey($key)) { continue }
    $relative = $pending[$key].Substring($root.Length + 1)
    $pending.Remove($key)
    foreach ($d in $message.params.diagnostics) {
        $severity = @{ 1 = 'error'; 2 = 'warning'; 3 = 'info'; 4 = 'hint' }[[int]$d.severity]
        $code = if ($d.PSObject.Properties['code']) { " [$($d.code)]" } else { '' }
        Write-Host ("{0}:{1}:{2}: {3} {4}{5}: {6}" -f $relative, ($d.range.start.line + 1), ($d.range.start.character + 1),
            $severity, $d.source, $code, $d.message)
        $problems++
    }
}

Send @{ jsonrpc = '2.0'; id = $script:nextId++; method = 'shutdown'; params = $null }
Send @{ jsonrpc = '2.0'; method = 'exit' }
if (-not $clangdProcess.WaitForExit(5000)) { $clangdProcess.Kill() }

Write-Host "LSP scan: $($paths.Count) file(s), $problems diagnostic(s)."
if ($problems) { exit 1 }
