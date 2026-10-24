<#
.SYNOPSIS
  Configure, build and test pqdaw inside the Visual Studio x64 developer environment.

.DESCRIPTION
  Tests: the default run (the test presets) skips the heavy sweeps (ctest label `slow`: doctest test suite
  "slow", T0.x-buildspeed) and the load-sensitive wall-clock checks (label `perf`, T0.x-flake). -Full adds the
  slow tier: run it before reporting a task done and before merging to main. -Perf runs only the perf label;
  it is opt-in and not part of -Full, because those checks depend on machine load.

  Compiler cache: when scripts/get-sccache.ps1 has installed sccache, CMake uses it as the compiler launcher
  (cmake/PqCompilerCache.cmake). This script points it at the cache shared by every checkout and worktree
  (%LOCALAPPDATA%\pqdaw\sccache, 20 GB cap; override with $env:PQ_SCCACHE_DIR) and prints its statistics
  after the build.

.EXAMPLE
  ./scripts/build.ps1                        # dev preset, build all, run the default test tier
  ./scripts/build.ps1 -Preset release
  ./scripts/build.ps1 -Full                  # default + slow tiers (everything except perf)
  ./scripts/build.ps1 -Preset release -Perf  # only the opt-in wall-clock perf checks (ctest label `perf`)
  ./scripts/build.ps1 -Target pq_tests_core -TestRegex core
  ./scripts/build.ps1 -NoTests
#>
param(
  [ValidateSet('dev', 'release')] [string] $Preset = 'dev',
  [string] $Target = '',
  [string] $TestRegex = '',
  [switch] $NoTests,
  [switch] $Full,
  [switch] $Perf,
  [switch] $Reconfigure
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'vsenv.ps1')

# sccache environment (read by the launcher and by the server it starts). One server per checkout, on a port
# derived from the checkout's path, with that checkout as its base directory: identical sources in two
# worktrees then hash identically and share the cache directory. Each build restarts it (fresh index of the shared cache); it exits after 30 idle minutes.
$sccacheDir = if ($env:PQ_SCCACHE_DIR) { $env:PQ_SCCACHE_DIR } else { Join-Path $env:LOCALAPPDATA 'pqdaw\sccache' }
$env:SCCACHE_DIR = $sccacheDir
$env:SCCACHE_CACHE_SIZE = '20G'
$env:SCCACHE_BASEDIRS = $root
$md5 = [Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))
$env:SCCACHE_SERVER_PORT = [string](14300 + ([BitConverter]::ToUInt16($md5, 0) % 4000))
$env:SCCACHE_IDLE_TIMEOUT = '1800'  # outlives a release build's long LTCG link tail, so the stats survive
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
  if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
  }

  $sccache = Get-Sccache $buildDir
  if ($sccache) {
    # Restart this checkout's server: a server indexes the shared cache directory when it starts and does not
    # see objects that other checkouts' servers store later, so a long-lived one would miss them. (A build
    # running concurrently in the same checkout compiles its in-flight files without the cache meanwhile.)
    & $sccache --stop-server *> $null
    & $sccache --start-server *> $null
  }

  $buildArgs = @('--build', '--preset', $Preset)
  if ($Target) { $buildArgs += @('--target', $Target) }
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
