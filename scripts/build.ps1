<#
.SYNOPSIS
  Configure, build and test pqdaw inside the Visual Studio x64 developer environment.

.DESCRIPTION
  Tests: the default run (the test presets) skips the heavy sweeps (ctest label `slow`: doctest test suite
  "slow", T0.x-buildspeed) and the load-sensitive wall-clock checks (label `perf`, T0.x-flake). -Full adds the
  slow tier: run it before reporting a task done and before merging to main. -Perf runs only the perf label;
  it is opt-in and not part of -Full, because those checks depend on machine load.

  Compiler cache: when scripts/get-sccache.ps1 has installed sccache, CMake uses it as the compiler launcher
  (cmake/PqCompilerCache.cmake). This script points it at this checkout's own cache directory
  (%LOCALAPPDATA%\pqdaw\sccache-v3\<checkout hash>, 6 GB cap; every checkout and worktree has its own, so a fresh
  worktree compiles cold; T0.x-buildspeed-fix3) and prints its statistics after the build. Override the root with
  $env:PQ_SCCACHE_DIR; scripts/clean-sccache.ps1 deletes the directories of removed worktrees. Dependency archives
  are downloaded once into %LOCALAPPDATA%\pqdaw\downloads, shared by every checkout.

.EXAMPLE
  ./scripts/build.ps1                        # dev preset, build all, run the default test tier
  ./scripts/build.ps1 -Preset release        # optimised, no LTCG (routine verification)
  ./scripts/build.ps1 -Preset ship -Target pq_bench_dsp -NoTests   # /GL + /LTCG: shipping builds, recorded benchmarks
  ./scripts/build.ps1 -Full                  # default + slow tiers (everything except perf)
  ./scripts/build.ps1 -Preset release -Perf  # only the opt-in wall-clock perf checks (ctest label `perf`)
  ./scripts/build.ps1 -Target pq_tests_core -TestRegex core
  ./scripts/build.ps1 -Full -Target pq_tests_document,pq_tests_views -TestRegex 'pq_tests_(document|views)'   # affected tests only
  ./scripts/build.ps1 -NoTests
#>
param(
  [ValidateSet('dev', 'release', 'ship')] [string] $Preset = 'dev',
  [string[]] $Target = @(),   # one or more targets: -Target a,b (from Git Bash: -Target a,b as one comma list)
  [string] $TestRegex = '',
  [switch] $NoTests,
  [switch] $Full,
  [switch] $Perf,
  [switch] $Reconfigure
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'vsenv.ps1')

# sccache environment (read by the launcher and by the server it starts). One server and one cache directory per
# checkout, the port derived from the checkout's path. No base directory (SCCACHE_BASEDIRS):
# with one, a hit could return an object compiled in another worktree together with that compile's
# /showIncludes output, so Ninja recorded the other worktree's headers as this object's dependencies and stopped
# rebuilding it when this checkout's headers changed (stale objects; T0.x-buildspeed-fix2). Entries are now keyed
# by this checkout's absolute paths: a hit only returns what this checkout compiled before. Each build restarts
# the server; it exits after 30 idle minutes.
# Each checkout has its own cache directory (T0.x-buildspeed-fix3): even without a base directory, an object
# compiled in another worktree was still replayed here once, with that worktree's /showIncludes (Ninja then
# tracked the wrong headers). Separate directories make that impossible. Delete the directories of removed
# worktrees with scripts/clean-sccache.ps1.
$md5 = [Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))
$checkoutKey = -join ($md5[0..5] | ForEach-Object { $_.ToString('x2') })
$sccacheRoot = if ($env:PQ_SCCACHE_DIR) { $env:PQ_SCCACHE_DIR } else { Join-Path $env:LOCALAPPDATA 'pqdaw\sccache-v3' }
$sccacheDir = Join-Path $sccacheRoot $checkoutKey
New-Item -ItemType Directory -Force $sccacheDir | Out-Null
Set-Content -Path (Join-Path $sccacheDir 'checkout.txt') -Value $root -NoNewline
$env:SCCACHE_DIR = $sccacheDir
$env:SCCACHE_CACHE_SIZE = '6G'
Remove-Item Env:SCCACHE_BASEDIRS -ErrorAction SilentlyContinue
$env:SCCACHE_SERVER_PORT = [string](14300 + ([BitConverter]::ToUInt16($md5, 0) % 4000))
$env:SCCACHE_IDLE_TIMEOUT = '1800'  # outlives a ship build's long LTCG link tail, so the stats survive
$env:SCCACHE_IGNORE_SERVER_IO_ERROR = '1'   # server unreachable: compile without the cache instead of failing

function Get-Sccache([string] $buildDir) {
  $cache = Join-Path $buildDir 'CMakeCache.txt'
  if (-not (Test-Path $cache)) { return '' }
  $line = Select-String -Path $cache -Pattern '^PQ_SCCACHE_LAUNCHER:INTERNAL=(.*)$' | Select-Object -First 1
  if ($line) { return $line.Matches[0].Groups[1].Value } else { return '' }
}

Push-Location $root
try {
  $buildDir = Join-Path $root "build/$Preset"
  # Build directories whose Ninja dependency log was written while worktrees shared cache entries can list
  # another worktree's headers as dependencies (see SCCACHE_BASEDIRS above). Drop that log once: Ninja then
  # rebuilds every object (from this checkout's cache entries where they exist) and records correct dependencies.
  $depsMarker = Join-Path $buildDir '.pq-deps-v3'
  if ((Test-Path (Join-Path $buildDir 'CMakeCache.txt')) -and -not (Test-Path $depsMarker)) {
    Remove-Item (Join-Path $buildDir '.ninja_deps') -ErrorAction SilentlyContinue
    Write-Host 'build.ps1: dependency log reset (T0.x-buildspeed-fix3); every object is rebuilt once'
  }
  # Reconfigure when CMakePresets.json changed after this build directory was configured: a preset's cache
  # variables (compiler / linker flags) only reach an existing CMakeCache.txt through `cmake --preset`.
  $cacheFile = Join-Path $buildDir 'CMakeCache.txt'
  $presetsChanged = (Test-Path $cacheFile) -and
    ((Get-Item (Join-Path $root 'CMakePresets.json')).LastWriteTimeUtc -gt (Get-Item $cacheFile).LastWriteTimeUtc)
  if ($Reconfigure -or $presetsChanged -or -not (Test-Path $cacheFile)) {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
  }

  New-Item -ItemType File -Force $depsMarker | Out-Null
  $sccache = Get-Sccache $buildDir
  if ($sccache) {
    # Restart this checkout's server, so it starts from a fresh index of its cache directory (kept from when the
    # cache was shared between checkouts; cheap). A build running concurrently in the same checkout compiles its
    # in-flight files without the cache meanwhile.
    & $sccache --stop-server *> $null
    & $sccache --start-server *> $null
  }

  $buildArgs = @('--build', '--preset', $Preset)
  $targets = @($Target | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
  if ($targets.Count -gt 0) { $buildArgs += @('--target') + $targets }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  cmake @buildArgs
  $buildExit = $LASTEXITCODE
  Write-Host ("build: {0:N1} s" -f $sw.Elapsed.TotalSeconds)
  $sccache = Get-Sccache $buildDir   # the build may have reconfigured
  if ($sccache) {
    $stats = & $sccache --show-stats 2>$null
    $keep = '^(Compile requests|Cache hits|Cache misses|Cache hits rate|Non-cacheable|Cache (read|write) errors|Cache location|Cache size|Max cache size)'
    Write-Host "sccache (port $env:SCCACHE_SERVER_PORT):"
    $stats | Where-Object { $_ -match $keep -and $_ -notmatch '\(C/C\+\+\)' } | ForEach-Object { Write-Host "  $_" }
  }
  if ($buildExit -ne 0) { throw "build failed ($buildExit)" }

  if (-not $NoTests) {
    # The test presets exclude the `slow` and `perf` labels. -Full and -Perf run from the build directory
    # instead, because a preset's label filter cannot be lifted from the command line.
    if ($Perf) {
      $testArgs = @('--test-dir', $buildDir, '-L', '^perf$', '--output-on-failure')
      $tier = 'perf label only'
    } elseif ($Full) {
      $testArgs = @('--test-dir', $buildDir, '-LE', '^perf$', '--output-on-failure', '--no-tests=error')
      $tier = 'full: default + slow; perf is opt-in via -Perf'
    } else {
      $testArgs = @('--preset', $Preset)
      $tier = 'default tier; -Full adds the slow label'
    }
    if ($TestRegex) { $testArgs += @('-R', $TestRegex) }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    ctest @testArgs
    $testExit = $LASTEXITCODE
    Write-Host ("tests ({0}): {1:N1} s" -f $tier, $sw.Elapsed.TotalSeconds)
    if ($testExit -ne 0) { throw "tests failed ($testExit)" }
  }
} finally {
  Pop-Location
}
