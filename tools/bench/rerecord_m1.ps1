<#
.SYNOPSIS
  Rebuilds the ship bench executables and re-records the M1 benchmark rows in bench/RESULTS.md (T1.11-fix1).

.DESCRIPTION
  The M1 rows (T1.10/T1.11: patch32 and the golden renders, the runner's SPSC rows, the selected atom rows) were
  recorded at 92-100 % system load on a dirty tree (M1 review M20). Run this on an idle machine (no other agent
  building) from a clean checkout of the commit to measure; it
    1. refuses a dirty tree and, unless -Indicative, a machine where compilers or linkers are running;
    2. builds pq_bench_render, pq_bench_core and pq_bench_dsp with the ship preset (/GL + /LTCG), benches only;
    3. runs them with --csv (pq_bench_render also records the proto-Operator SPEC §13 calibration rows: operator/);
    4. appends the rows with tools/bench/append_results.py, which refuses rows above 25 % system load unless
       -Indicative (then labelled "indicative").
  CSVs go to build/ship/bench/m1-rerecord (not committed). Commit bench/RESULTS.md afterwards.

.EXAMPLE
  ./tools/bench/rerecord_m1.ps1                       # idle machine: baseline rows
  ./tools/bench/rerecord_m1.ps1 -DryRun               # build and run, print the rows, append nothing
  ./tools/bench/rerecord_m1.ps1 -Indicative -Note "machine shared"   # record anyway, labelled indicative
#>
param(
  [string] $Label = 'T1.11-fix1 M1 re-record',
  [string] $Note = '',
  [switch] $Indicative,
  [switch] $DryRun,
  [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
  $dirty = git status --porcelain --untracked-files=all | Where-Object { $_ -and ($_.Substring(3) -ne 'bench/RESULTS.md') }
  if ($dirty) { throw "rerecord_m1: the working tree is dirty; commit first:`n$($dirty -join "`n")" }

  $busy = Get-Process -Name cl, link, lld-link, ninja, clang, clang-cl -ErrorAction SilentlyContinue
  if ($busy -and -not $Indicative) {
    throw "rerecord_m1: $($busy.Count) compiler/linker processes are running: the machine is not idle (wait, or pass -Indicative)"
  }

  $targets = 'pq_bench_render', 'pq_bench_core', 'pq_bench_dsp'
  if (-not $SkipBuild) {
    foreach ($t in $targets) {
      & (Join-Path $root 'scripts/build.ps1') -Preset ship -Target $t -NoTests
      if ($LASTEXITCODE -ne 0) { throw "rerecord_m1: ship build of $t failed" }
    }
  }

  $out = Join-Path $root 'build/ship/bench/m1-rerecord'
  New-Item -ItemType Directory -Force $out | Out-Null
  $bench = Join-Path $root 'build/ship/bench'
  # name, extra arguments, --config text, --note text. Filters: the rows T1.10/T1.11 recorded.
  $runs = @(
    @{ exe = 'pq_bench_render'; args = @(); config = 'block 64, 48 kHz';
       note = 'patch32 = 32-op voice chain via VoiceEngine, held notes; render = whole 1 s offline render, compile excluded; operator = proto-Operator x64 through ProgramHost (SPEC 13 calibration)' },
    @{ exe = 'pq_bench_core'; args = @('--filter', '^(spsc/push\+pop|cross/spsc round trip u64$)'); config = '';
       note = 'M1 re-record of the T1.11 SPSC rows; cross rows 11 runs/variant, pinned' },
    @{ exe = 'pq_bench_dsp'; args = @('--filter', '^atom/(add|mul|tanh|fast_tanh|z1|delay|phasor|sine_hz|wavetable|noise)$'); config = '';
       note = 'M1 re-record of the T1.11 atom rows' }
  )
  foreach ($r in $runs) {
    $csv = Join-Path $out "$($r.exe).csv"
    & (Join-Path $bench "$($r.exe).exe") @($r.args) --csv $csv
    if ($LASTEXITCODE -ne 0) { throw "rerecord_m1: $($r.exe) failed ($LASTEXITCODE)" }
    $appendArgs = @((Join-Path $root 'tools/bench/append_results.py'), $csv, '--label', $Label)
    if ($r.config) { $appendArgs += @('--config', $r.config) }
    $n = $r.note + $(if ($Note) { "; $Note" } else { '' })
    $appendArgs += @('--note', $n)
    if ($Indicative) { $appendArgs += '--indicative' }
    if ($DryRun) { $appendArgs += '--dry-run' }
    python @appendArgs
    if ($LASTEXITCODE -ne 0) { throw "rerecord_m1: append_results.py refused $($r.exe)'s rows (see above)" }
  }
  if (-not $DryRun) { Write-Host 'rerecord_m1: rows appended to bench/RESULTS.md; review and commit them' }
} finally {
  Pop-Location
}
