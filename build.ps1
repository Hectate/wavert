<#
.SYNOPSIS
    Builds this project's Wavert (.wvt) programs into native executables.

.DESCRIPTION
    Uses wvc, the Wavert compiler, and Zig as the C compiler:

    1. Finds Zig: -Zig, else the ZIG environment variable, else zig on PATH.
    2. Builds wvc from its seed, wvc\wvc.c, into .wvt\wvc.exe, if it is
       missing or older than the seed.
    3. Builds every program in src\ into bin\<name>.exe. A program is a .wvt
       file whose last statement is ;exit (a library ends with ;end).

    wvc writes the generated C beside each executable (bin\<name>.c).

.EXAMPLE
    .\build.ps1
    Builds every program.

.EXAMPLE
    .\build.ps1 -Program primes -Run
    Builds src\primes.wvt (or the program named primes) and runs it.

.EXAMPLE
    .\build.ps1 -Clean
    Deletes bin\ and the cached compiler, then builds everything.
#>
[CmdletBinding()]
param(
    # The zig executable (the file, not its folder).
    [string]$Zig = $(if ($env:ZIG) { $env:ZIG } else { 'zig' }),
    # Where the .wvt sources are, and where the executables go.
    [string]$Source = 'src',
    [string]$Out = 'bin',
    # Build only these programs, by name (primes) or path (src\primes.wvt).
    [string[]]$Program = @(),
    # Run the program after building it. Needs exactly one program.
    [switch]$Run,
    # Arguments for the program, with -Run.
    [string[]]$RunArgs = @(),
    # Delete bin\ and the cached compiler first.
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$toolDir = Join-Path $root '.wvt'

function Fail([string]$message) {
    Write-Host "build: $message" -ForegroundColor Red
    exit 1
}

# ---- Zig ----
if ($Zig -match '[\\/]') {
    if (-not (Test-Path -LiteralPath $Zig -PathType Leaf)) {
        Fail "Zig not found at '$Zig'. Point -Zig or ZIG at the zig executable (the file, not its folder)."
    }
    $zigPath = (Resolve-Path -LiteralPath $Zig).Path
} else {
    $cmd = Get-Command $Zig -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $cmd) {
        Fail "Zig not found. Install it from https://ziglang.org/download/, then put its folder on PATH, set ZIG to zig.exe, or pass -Zig C:\path\to\zig.exe."
    }
    $zigPath = $cmd.Source
}

# ---- The Wavert toolchain ----
$seed = Join-Path $root 'wvc\wvc.c'
$runtime = Join-Path $root 'runtime\wvt.c'
if (-not (Test-Path -LiteralPath $seed) -or -not (Test-Path -LiteralPath $runtime)) {
    Fail "Missing wvc\wvc.c or runtime\wvt.c."
}

# ---- Clean ----
$outDir = Join-Path $root $Out
if ($Clean) {
    foreach ($dir in @($outDir, $toolDir)) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    }
}

# ---- wvc ----
$wvc = Join-Path $toolDir 'wvc.exe'
$stale = -not (Test-Path -LiteralPath $wvc) -or
    (Get-Item -LiteralPath $wvc).LastWriteTimeUtc -lt (Get-Item -LiteralPath $seed).LastWriteTimeUtc
if ($stale) {
    New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
    Write-Host "Building wvc from $seed (the first build with a new Zig takes about half a minute)..."
    & $zigPath cc -O2 -o $wvc $seed
    if ($LASTEXITCODE -ne 0) { Fail "zig cc could not build wvc (exit code $LASTEXITCODE)." }
}

# ---- Programs ----
function Test-Program([string]$path) {
    $lines = Get-Content -LiteralPath $path -Encoding UTF8
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        $line = ($lines[$i] -replace '//.*$', '').Trim()
        if ($line -eq '' -or $line -eq ';') { continue }
        return $line -match '(^|;)\s*exit\b[^;]*$'
    }
    return $false
}

$srcDir = Join-Path $root $Source
if (-not (Test-Path -LiteralPath $srcDir)) { Fail "No source folder '$srcDir'." }
$all = @(Get-ChildItem -LiteralPath $srcDir -Recurse -Filter '*.wvt' | Where-Object { Test-Program $_.FullName })

if ($Program.Count -gt 0) {
    $chosen = @()
    foreach ($p in $Program) {
        $match = @($all | Where-Object { $_.BaseName -eq $p -or $_.FullName -eq (Join-Path $root $p) -or $_.FullName -eq $p })
        if ($match.Count -eq 0) { Fail "No program '$p' in $Source (a program's last statement is ;exit)." }
        $chosen += $match
    }
    $all = $chosen
}
if ($all.Count -eq 0) { Fail "No programs in $Source (a program's last statement is ;exit)." }
if ($Run -and $all.Count -ne 1) { Fail '-Run needs exactly one program; choose it with -Program.' }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$failed = @()
foreach ($file in $all) {
    $exe = Join-Path $outDir ($file.BaseName + '.exe')
    Write-Host "wvc build $($file.FullName.Substring($root.Length + 1)) -> $($exe.Substring($root.Length + 1))"
    & $wvc build $file.FullName -o $exe --cc $zigPath --runtime $runtime
    if ($LASTEXITCODE -ne 0) { $failed += $file.Name }
}
if ($failed.Count -gt 0) { Fail "failed: $($failed -join ', ')" }

if ($Run) {
    $exe = Join-Path $outDir ($all[0].BaseName + '.exe')
    & $exe @RunArgs
    exit $LASTEXITCODE
}
