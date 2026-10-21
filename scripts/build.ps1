<#
.SYNOPSIS
  Configure, build and test pqdaw inside the Visual Studio x64 developer environment.

.EXAMPLE
  ./scripts/build.ps1                       # dev preset, build all, run all tests
  ./scripts/build.ps1 -Preset release
  ./scripts/build.ps1 -Target pq_tests_core -TestRegex core
  ./scripts/build.ps1 -NoTests
#>
param(
  [ValidateSet('dev', 'release')] [string] $Preset = 'dev',
  [string] $Target = '',
  [string] $TestRegex = '',
  [switch] $NoTests,
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
    $testArgs = @('--preset', $Preset)
    if ($TestRegex) { $testArgs += @('-R', $TestRegex) }
    ctest @testArgs
    if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
  }
} finally {
  Pop-Location
}
