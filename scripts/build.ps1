<#
.SYNOPSIS
  Configure, build and test pqdaw inside the Visual Studio x64 developer environment.

.DESCRIPTION
  Presets: `check` (the default; release optimisation with asserts on: definition-of-done verification), `dev`
  (unoptimised, for debugging), `release` (asserts off, for perf A/B) and `ship` (LTCG; recorded benchmarks).

  Tests: the default run (the test presets) skips the heavy sweeps (ctest label `slow`: doctest test suite
  "slow", T0.x-buildspeed) and the load-sensitive wall-clock checks (label `perf`, T0.x-flake). -Full adds the
  slow tier. -Perf runs only the perf label; it is opt-in and not part of -Full, because those checks depend on
  machine load. CTest runs $env:CTEST_PARALLEL_LEVEL entries at once (default 8); the slowest executables are
  split into shards (pq_add_test SHARDS / SLOW_SHARDS) so they run in parallel too.

  -Affected (T0.x-buildspeed2): scripts/affected.ps1 maps this branch's changes (against origin/main, plus
  uncommitted files) to the targets to build and the tests to run, default + slow tiers. This is the worker's
  verification run.

  Compiler cache: when scripts/get-sccache.ps1 has installed sccache, CMake uses it as the compiler launcher
  (cmake/PqCompilerCache.cmake). Every checkout shares one cache directory (%LOCALAPPDATA%\pqdaw\sccache-shared,
  20 GB cap; override with $env:PQ_SCCACHE_DIR) and one sccache server, whose base directories are all known
  checkout roots, so a fresh worktree gets hits from the others. A hit replays the /showIncludes output of the
  checkout that filled the entry; after every build the foreign-dependency check (scripts/NinjaDeps.cs) rewrites
  such paths in Ninja's dependency log to this checkout, recompiles (SCCACHE_RECACHE=1) any object it cannot
  repair, and fails the run if foreign dependencies survive. Dependency archives are downloaded once into
  %LOCALAPPDATA%\pqdaw\downloads.

.EXAMPLE
  ./scripts/build.ps1 -Affected                # worker verification: affected targets, default + slow tests (check)
  ./scripts/build.ps1 -Target pq_tests_dsp -TestRegex pq_tests_dsp   # iterate on one test executable
  ./scripts/build.ps1 -Full                    # whole suite, default + slow (orchestrator, milestone boundaries)
  ./scripts/build.ps1 -Preset dev -Target pq_tests_core -NoTests      # unoptimised build for the debugger
  ./scripts/build.ps1 -Preset release -Perf    # only the opt-in wall-clock perf checks (ctest label `perf`)
  ./scripts/build.ps1 -Preset ship -Target pq_bench_dsp -NoTests      # /GL + /LTCG: recorded benchmarks
#>
param(
  [ValidateSet('check', 'dev', 'release', 'ship')] [string] $Preset = 'check',
  [string[]] $Target = @(),   # one or more targets: -Target a,b (from Git Bash: -Target a,b as one comma list)
  [string] $TestRegex = '',
  [switch] $NoTests,
  [switch] $Full,
  [switch] $Perf,
  [switch] $Affected,
  [string] $Base = 'origin/main',   # -Affected: the branch point to diff against
  [switch] $Reconfigure
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'vsenv.ps1')
$swTotal = [Diagnostics.Stopwatch]::StartNew()

# --- shared compiler cache ------------------------------------------------------------------------------------
# One directory and one server for every checkout. The base directories (SCCACHE_BASEDIRS, read when the server
# starts) are every checkout root git knows, longest first, so the same source in two worktrees hashes the same.
# The server is restarted only when this checkout is missing from the running server's list (a new worktree);
# a build running elsewhere at that moment compiles its in-flight files without the cache.
$sccacheDir = if ($env:PQ_SCCACHE_DIR) { $env:PQ_SCCACHE_DIR } else { Join-Path $env:LOCALAPPDATA 'pqdaw\sccache-shared' }
New-Item -ItemType Directory -Force $sccacheDir | Out-Null
$env:SCCACHE_DIR = $sccacheDir
$env:SCCACHE_CACHE_SIZE = '20G'
$env:SCCACHE_SERVER_PORT = '14299'
$env:SCCACHE_IDLE_TIMEOUT = '3600'
$env:SCCACHE_IGNORE_SERVER_IO_ERROR = '1'   # server unreachable: compile without the cache instead of failing
Remove-Item Env:SCCACHE_RECACHE -ErrorAction SilentlyContinue

$checkouts = @(git -C $root worktree list --porcelain | Where-Object { $_ -like 'worktree *' } |
  ForEach-Object { [IO.Path]::GetFullPath($_.Substring(9)).TrimEnd('\') })
if ($checkouts -notcontains $root) { $checkouts += $root }

function Get-Sccache([string] $buildDir) {
  $cache = Join-Path $buildDir 'CMakeCache.txt'
  if (-not (Test-Path $cache)) { return '' }
  $line = Select-String -Path $cache -Pattern '^PQ_SCCACHE_LAUNCHER:INTERNAL=(.*)$' | Select-Object -First 1
  if ($line) { return $line.Matches[0].Groups[1].Value } else { return '' }
}

function Start-SharedSccache([string] $sccache) {
  $state = Join-Path $sccacheDir 'server-basedirs.txt'
  $have = if (Test-Path $state) { @(Get-Content $state | Where-Object { $_ }) } else { @() }
  # Probe the port directly: any sccache client call would start a server with this process's environment.
  $tcp = [Net.Sockets.TcpClient]::new()
  try { $running = $tcp.ConnectAsync('127.0.0.1', [int]$env:SCCACHE_SERVER_PORT).Wait(300) } catch { $running = $false } finally { $tcp.Dispose() }
  if ($running -and $have -contains $root) { $env:SCCACHE_BASEDIRS = ($have -join ';'); return }
  $want = @(@($have) + $checkouts | Where-Object { Test-Path $_ } | Sort-Object -Unique | Sort-Object Length -Descending)
  $env:SCCACHE_BASEDIRS = ($want -join ';')
  & $sccache --stop-server *> $null
  & $sccache --start-server *> $null
  Set-Content -Path $state -Value $want
  Write-Host "build.ps1: sccache server (port $env:SCCACHE_SERVER_PORT) restarted with $($want.Count) base directories"
}

function Get-SccacheCounts([string] $sccache) {
  try {
    $j = (& $sccache --show-stats --stats-format json 2>$null) | ConvertFrom-Json
    $sum = { param($o) if ($o -and $o.counts) { ($o.counts.PSObject.Properties | Measure-Object Value -Sum).Sum } else { 0 } }
    return @{ hits = & $sum $j.stats.cache_hits; misses = & $sum $j.stats.cache_misses; requests = [int64]$j.stats.compile_requests }
  } catch { return $null }
}

# --- foreign-dependency check (Phase 1b of the 3d-sand-voxel build, adapted) ----------------------------------
Add-Type -Path (Join-Path $PSScriptRoot 'NinjaDeps.cs')
function Repair-ForeignDeps([string] $buildDir, [string] $label) {
  $r = [Pq.NinjaDeps]::Fix($buildDir, $root, [string[]]$checkouts, $true)
  if ($r.ForeignObjects -gt 0) {
    Write-Host ("build.ps1: foreign-dependency check ({0}): {1} of {2} objects had dependencies in {3}; {4} rewritten to this checkout, {5} not repairable" -f
      $label, $r.ForeignObjects, $r.Objects, ($r.ForeignRoots -join ', '), $r.Rewritten, $r.Unmappable.Count)
  } elseif ($r.Note) {
    Write-Host "build.ps1: foreign-dependency check ($label): $($r.Note)"
  } else {
    Write-Host "build.ps1: foreign-dependency check ($label): $($r.Objects) objects, none foreign"
  }
  return $r
}

Push-Location $root
try {
  $buildDir = Join-Path $root "build/$Preset"
  # Build directories from before T0.x-buildspeed2 (per-checkout caches) have correct dependency logs: keep them.
  # Reconfigure when CMakePresets.json changed after this build directory was configured: a preset's cache
  # variables (compiler / linker flags) only reach an existing CMakeCache.txt through `cmake --preset`.
  $cacheFile = Join-Path $buildDir 'CMakeCache.txt'
  $presetsChanged = (Test-Path $cacheFile) -and
    ((Get-Item (Join-Path $root 'CMakePresets.json')).LastWriteTimeUtc -gt (Get-Item $cacheFile).LastWriteTimeUtc)
  if ($Reconfigure -or $presetsChanged -or -not (Test-Path $cacheFile) -or -not (Test-Path (Join-Path $buildDir 'pq_graph.json'))) {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
  }

  $targets = @($Target | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
  $runTests = -not $NoTests
  $testRegex = $TestRegex
  $fullTier = $Full
  if ($Affected) {
    $plan = & (Join-Path $PSScriptRoot 'affected.ps1') -Base $Base -BuildDir $buildDir
    $fullTier = $true
    if ($plan.All) {
      $targets = @()
    } elseif (-not $plan.Targets -and -not $plan.TestRegex) {
      Write-Host 'build.ps1 -Affected: nothing to build or test for these changes'
      return
    } else {
      $targets = @($plan.Targets)
      $testRegex = $plan.TestRegex
      if (-not $testRegex) { $runTests = $false }
    }
  }

  $sccache = Get-Sccache $buildDir
  if ($sccache) { Start-SharedSccache $sccache }
  [void](Repair-ForeignDeps $buildDir 'before the build')
  $before = if ($sccache) { Get-SccacheCounts $sccache } else { $null }

  $buildArgs = @('--build', '--preset', $Preset)
  if ($targets.Count -gt 0) { $buildArgs += @('--target') + $targets }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  cmake @buildArgs
  $buildExit = $LASTEXITCODE
  Write-Host ("build: {0:N1} s" -f $sw.Elapsed.TotalSeconds)
  $sccache = Get-Sccache $buildDir   # the build may have reconfigured
  if ($sccache -and $before) {
    $after = Get-SccacheCounts $sccache
    if ($after) {
      $h = $after.hits - $before.hits; $m = $after.misses - $before.misses
      $rate = if ($h + $m -gt 0) { 100.0 * $h / ($h + $m) } else { 0 }
      Write-Host ("sccache (shared, port {0}): {1} hits, {2} misses ({3:N1} % hits) during this build; includes concurrent builds of other checkouts" -f
        $env:SCCACHE_SERVER_PORT, $h, $m, $rate)
    }
  }

  # Repair before anything else reads the dependency log, also after a failed build (its objects are recorded).
  $r = Repair-ForeignDeps $buildDir 'after the build'
  if ($r.Unmappable.Count -gt 0) {
    foreach ($o in $r.Unmappable) { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $buildDir $o) }
    Write-Host "build.ps1: recompiling $($r.Unmappable.Count) objects without the cache (SCCACHE_RECACHE=1)"
    $env:SCCACHE_RECACHE = '1'
    try { cmake @buildArgs; $buildExit = $LASTEXITCODE } finally { Remove-Item Env:SCCACHE_RECACHE -ErrorAction SilentlyContinue }
    $r = Repair-ForeignDeps $buildDir 'after the recompile'
    if ($r.Unmappable.Count -gt 0) {
      throw "build.ps1: FOREIGN DEPENDENCIES SURVIVED A RECACHE ($($r.Unmappable -join ', ')); delete build/$Preset/.ninja_deps or the cache at $sccacheDir"
    }
  }
  if ($buildExit -ne 0) { throw "build failed ($buildExit)" }

  if ($runTests) {
    $jobs = if ($env:CTEST_PARALLEL_LEVEL) { $env:CTEST_PARALLEL_LEVEL } else { '8' }
    # The test presets exclude the `slow` and `perf` labels. -Full and -Perf run from the build directory
    # instead, because a preset's label filter cannot be lifted from the command line.
    if ($Perf) {
      $testArgs = @('--test-dir', $buildDir, '-L', '^perf$', '--output-on-failure')
      $tier = 'perf label only'
    } elseif ($fullTier) {
      $testArgs = @('--test-dir', $buildDir, '-LE', '^perf$', '--output-on-failure', '--no-tests=error', '-j', $jobs)
      $tier = 'full: default + slow; perf is opt-in via -Perf'
    } else {
      $testArgs = @('--preset', $Preset, '-j', $jobs)
      $tier = 'default tier; -Full adds the slow label'
    }
    if ($testRegex) { $testArgs += @('-R', $testRegex) }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    ctest @testArgs
    $testExit = $LASTEXITCODE
    Write-Host ("tests ({0}): {1:N1} s" -f $tier, $sw.Elapsed.TotalSeconds)
    if ($testExit -ne 0) { throw "tests failed ($testExit)" }
  }
  Write-Host ("build.ps1: done in {0:N1} s" -f $swTotal.Elapsed.TotalSeconds)
} finally {
  Pop-Location
}
