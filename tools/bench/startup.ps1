<#
.SYNOPSIS
  T0.7-fix6: start pqdaw N times, take each run's first-frame time measured from process creation, print the median.

.DESCRIPTION
  Each run starts pqdaw with PQ_STARTUP_EXIT=1 (it quits right after the first presented frame) and reads the line
  "pqdaw: first frame X ms after main() (Y ms after process creation)". The starts are "cold-ish": the executable and
  its DLLs are in the OS file cache after the first run, the Vulkan pipeline cache in the user settings directory
  (%LOCALAPPDATA%\pqdaw\vk_pipeline_cache.bin) is warm after the first run unless -ColdPipelineCache deletes it before
  every run. Prints the CPU load before and after: numbers taken while other builds run are indicative only.

.EXAMPLE
  ./tools/bench/startup.ps1                                   # build/release pqdaw, 10 runs, the empty fixture project
  ./tools/bench/startup.ps1 -Preset ship -Runs 10 -Phases     # + the per-phase table of the median run
  ./tools/bench/startup.ps1 -Project tests/views/fixtures/arrangement
#>
param(
  [ValidateSet('release', 'ship', 'dev')] [string] $Preset = 'release',
  [string] $Exe = '',
  [int] $Runs = 10,
  [string] $Project = 'tests/views/fixtures/empty',
  [switch] $Phases,
  [switch] $ColdPipelineCache
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Exe) { $Exe = Join-Path $root "build/$Preset/src/app/pqdaw.exe" }
if (-not (Test-Path $Exe)) { throw "pqdaw not found at $Exe (build it: ./scripts/build.ps1 -Preset $Preset -Target pqdaw -NoTests)" }
$projectPath = if ([IO.Path]::IsPathRooted($Project)) { $Project } else { Join-Path $root $Project }
$cacheFile = Join-Path $env:LOCALAPPDATA 'pqdaw\vk_pipeline_cache.bin'

function Get-Load { [int](Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average }

$env:PQ_STARTUP_EXIT = '1'
$env:PQ_STARTUP_LOG = '1'
$loadBefore = Get-Load
$results = @()
for ($i = 1; $i -le $Runs; $i++) {
  if ($ColdPipelineCache -and (Test-Path $cacheFile)) { Remove-Item $cacheFile -Force }
  $out = & $Exe --project $projectPath 2>&1 | Out-String
  $m = [regex]::Match($out, 'first frame ([\d.]+) ms after main\(\) \(([\d.]+) ms after process creation\)')
  if (-not $m.Success) {
    Write-Host "run ${i}: no first-frame line (exit $LASTEXITCODE)`n$out"
    continue
  }
  $ms = [double]$m.Groups[2].Value
  $results += [pscustomobject]@{ Run = $i; Ms = $ms; MainMs = [double]$m.Groups[1].Value; Output = $out }
  Write-Host ("run {0,2}: first frame {1,7:N1} ms after process creation ({2,7:N1} ms after main)" -f $i, $ms, [double]$m.Groups[1].Value)
}
$loadAfter = Get-Load
Remove-Item Env:PQ_STARTUP_EXIT, Env:PQ_STARTUP_LOG -ErrorAction SilentlyContinue
if ($results.Count -eq 0) { throw 'no successful runs' }
$sorted = $results | Sort-Object Ms
$median = if ($sorted.Count % 2) { $sorted[[int][math]::Floor($sorted.Count / 2)].Ms } else { ($sorted[$sorted.Count / 2 - 1].Ms + $sorted[$sorted.Count / 2].Ms) / 2 }
Write-Host ("median first frame over {0} runs: {1:N1} ms after process creation (min {2:N1}, max {3:N1}); CPU load {4} % before, {5} % after" -f `
    $results.Count, $median, $sorted[0].Ms, $sorted[-1].Ms, $loadBefore, $loadAfter)
if ($Phases) {
  $mid = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]
  Write-Host "phases of run $($mid.Run) ($($mid.Ms) ms):"
  $lines = $mid.Output -split "`n"
  $at = [array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match '^startup phases' })
  if ($at -ge 0) { $lines[($at + 1)..($lines.Count - 1)] | Where-Object { $_ -match '^\s+\S' } | ForEach-Object { Write-Host $_.TrimEnd() } }
}
