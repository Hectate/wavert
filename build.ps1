<#
.SYNOPSIS
    Builds this project's Γ-- (.wah) programs into native executables.

.DESCRIPTION
    Uses wahc, the Γ-- compiler, and Zig as the C compiler:

    1. Finds Zig: -Zig, else the ZIG environment variable, else zig on PATH.
    2. Builds wahc from this repo's wah\ folder (or -WahHome) into
       .wah\wahc.exe, if it is missing or older than wah\wahc\wahc.c.
    3. Builds every program in src\ into bin\<name>.exe. A program is a .wah

    wahc writes the generated C beside each executable (bin\<name>.c).

.EXAMPLE
    .\build.ps1
    Builds every program.

.EXAMPLE
    .\build.ps1 -Program mansion -Run -RunArgs 'save1'
    Builds src\mansion.wah (or the program named mansion) and runs it with one argument.

.EXAMPLE
    .\build.ps1 -Clean
    Deletes bin\ and the cached compiler, then builds everything.
#>
[CmdletBinding()]
param(
    # The Γ-- toolchain: it has wahc\wahc.c and runtime\wah.c. Defaults to this repo's wah\ folder.
    [string]$WahHome = '',
    # The zig executable (the file, not its folder).
    [string]$Zig = $(if ($env:ZIG) { $env:ZIG } else { 'zig' }),
    # Where the .wah sources are, and where the executables go.
    [string]$Source = 'src',
    [string]$Out = 'bin',
    # Build only these programs, by name (mansion) or path (src\mansion.wah).
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
$toolDir = Join-Path $root '.wah'
if (-not $WahHome) { $WahHome = Join-Path $root 'wah' }

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

# ---- The Γ-- toolchain ----
$seed = Join-Path $WahHome 'wahc\wahc.c'
$runtime = Join-Path $WahHome 'runtime\wah.c'
if (-not (Test-Path -LiteralPath $seed) -or -not (Test-Path -LiteralPath $runtime)) {
    Fail "No wah toolchain at '$WahHome' (expected wahc\wahc.c and runtime\wah.c). Pass -WahHome."
}

# ---- Clean ----
$outDir = Join-Path $root $Out
if ($Clean) {
    foreach ($dir in @($outDir, $toolDir)) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    }
}

# ---- wahc ----
$wahc = Join-Path $toolDir 'wahc.exe'
$stale = -not (Test-Path -LiteralPath $wahc) -or
    (Get-Item -LiteralPath $wahc).LastWriteTimeUtc -lt (Get-Item -LiteralPath $seed).LastWriteTimeUtc
if ($stale) {
    New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
    Write-Host "Building wahc from $seed (the first build with a new Zig takes about half a minute)..."
    & $zigPath cc -O2 -o $wahc $seed
    if ($LASTEXITCODE -ne 0) { Fail "zig cc could not build wahc (exit code $LASTEXITCODE)." }
}

# ---- Programs ----
function Test-Program([string]$path) {
    $lines = Get-Content -LiteralPath $path -Encoding UTF8
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        $line = ($lines[$i] -replace '//.*$', '').Trim()
        if ($line -eq '' -or $line -eq ';') { continue }
        return $line -match '!invited\s*;*$'
    }
    return $false
}

$srcDir = Join-Path $root $Source
if (-not (Test-Path -LiteralPath $srcDir)) { Fail "No source folder '$srcDir'." }
$all = @(Get-ChildItem -LiteralPath $srcDir -Recurse -Filter '*.wah' | Where-Object { Test-Program $_.FullName })

if ($Program.Count -gt 0) {
    $chosen = @()
    foreach ($p in $Program) {
        $match = @($all | Where-Object { $_.BaseName -eq $p -or $_.FullName -eq (Join-Path $root $p) -or $_.FullName -eq $p })
        if ($match.Count -eq 0) { Fail "No program '$p' in $Source (a program's last statement is ;!invited)." }
        $chosen += $match
    }
    $all = $chosen
}
if ($all.Count -eq 0) { Fail "No programs in $Source (a program's last statement is ;!invited)." }
if ($Run -and $all.Count -ne 1) { Fail '-Run needs exactly one program; choose it with -Program.' }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$failed = @()
foreach ($file in $all) {
    $exe = Join-Path $outDir ($file.BaseName + '.exe')
    Write-Host "wahc build $($file.FullName.Substring($root.Length + 1)) -> $($exe.Substring($root.Length + 1))"
    & $wahc build $file.FullName -o $exe --cc $zigPath --runtime $runtime
    if ($LASTEXITCODE -ne 0) { $failed += $file.Name }
}
if ($failed.Count -gt 0) { Fail "failed: $($failed -join ', ')" }

if ($Run) {
    $exe = Join-Path $outDir ($all[0].BaseName + '.exe')
    & $exe @RunArgs
    exit $LASTEXITCODE
}
