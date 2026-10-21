<#
.SYNOPSIS
  Configure, build and test pqdaw inside the Visual Studio x64 developer environment.

.EXAMPLE
  ./scripts/build.ps1                       # dev preset, build all, run all tests
  ./scripts/build.ps1 -Preset release
  ./scripts/build.ps1 -Target pq_tests_core -TestRegex core
  ./scripts/build.ps1 -NoTests
  ./scripts/build.ps1 -Preset release -Perf  # only the opt-in wall-clock perf checks (ctest label `perf`)

  The default test run (the test presets) excludes the `perf` label: those checks depend on machine load.
#>
param(
  [ValidateSet('dev', 'release')] [string] $Preset = 'dev',
  [string] $Target = '',
  [string] $TestRegex = '',
  [switch] $NoTests,
  [switch] $Perf,
  [switch] $Reconfigure
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'vsenv.ps1')

Push-Location $root
try {
  $buildDir = Join-Path $root "build/$Preset"
  if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
  }

  $buildArgs = @('--build', '--preset', $Preset)
  if ($Target) { $buildArgs += @('--target', $Target) }
  cmake @buildArgs
  if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

  if (-not $NoTests) {
    # -Perf: the perf label only (the preset's label filter would exclude it, so run from the build dir).
    $testArgs = if ($Perf) { @('--test-dir', $buildDir, '-L', 'perf', '--output-on-failure') } else { @('--preset', $Preset) }
    if ($TestRegex) { $testArgs += @('-R', $TestRegex) }
    ctest @testArgs
    if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
  }
} finally {
  Pop-Location
}
