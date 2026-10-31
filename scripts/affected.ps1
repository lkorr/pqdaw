<#
.SYNOPSIS
  Which targets to build and which CTest entries to run for this branch's changes (T0.x-buildspeed2).

.DESCRIPTION
  Changed files: `git diff --name-only <Base>...HEAD` plus uncommitted and untracked files (or -Files). Each file
  maps to a set of test executables through the target graph that CMake writes at configure time
  (<build>/pq_graph.json, cmake/PqModule.cmake: every module, app executable, test and bench with its source
  directories and link dependencies), so the mapping follows the CMake files and cannot go stale:

    docs, *.md, reference/, .claude/, other scripts          nothing
    CMake files, presets, scripts/build*, vsenv, affected,   everything (-Full whole suite)
      NinjaDeps.cs, src/core, tests/CMakeLists.txt,
      tests/*.cmake, the shared test main
    src/<module>/..., tools/<module>/...                     the tests (and pqdaw_* app tests) of every executable
                                                             that links that module, directly or transitively
    tests/<dir>/... (sources, fixtures, goldens)             the test executables built from tests/<dir>
    stdlib/...                                               test executables whose test sources read stdlib/
                                                             (grep), plus the pqdaw app tests
    assets/...                                               everything that links pq_ui
    bench/<dir>/...                                          build that dir's benches; bench/support: all benches

  Judgment (compartmentalisation): the DSP kernels (src/dsp/src/kernels_*, tables, simd.h) only run through the
  engine's interpreter, the JIT and graph constant folding. Changes there run the tests of executables that link
  pq_engine, plus pq_tests_dsp and pq_tests_graph; ui / views / document / library / tuning tests link pq_dsp only
  for its metadata, spectrum and resampling code and do not run kernels. A change to a dsp public header still
  BUILDS every dependent (compile errors), it only skips running the ones that cannot observe it.

  Use from scripts/build.ps1 -Affected; standalone it prints the plan.

.EXAMPLE
  ./scripts/affected.ps1
  ./scripts/affected.ps1 -Files src/dsp/src/kernels_math.cpp
#>
param(
  [string] $Base = 'origin/main',
  [string] $BuildDir = '',
  [string[]] $Files = $null,
  [switch] $Quiet
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build/check' }

function Say([string] $s) { if (-not $Quiet) { Write-Host $s } }

# --- changed files --------------------------------------------------------------------------------------------
if ($null -eq $Files) {
  Push-Location $root
  try {
    $mergeBase = git merge-base $Base HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $mergeBase) { throw "affected.ps1: no merge base with $Base (git fetch?)" }
    $Files = @(git diff --name-only $mergeBase) + @(git ls-files --others --exclude-standard)
  } finally { Pop-Location }
}
$Files = @($Files | ForEach-Object { ($_ -replace '\\', '/').Trim() } | Where-Object { $_ } | Sort-Object -Unique)

# --- target graph ---------------------------------------------------------------------------------------------
$graphFile = Join-Path $BuildDir 'pq_graph.json'
if (-not (Test-Path $graphFile)) { throw "affected.ps1: $graphFile missing; configure first (build.ps1 does)" }
$graph = @{}
(Get-Content -Raw $graphFile | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $graph[$_.Name] = $_.Value }

$closure = @{}
function Get-Closure([string] $t) {
  if ($closure.ContainsKey($t)) { return $closure[$t] }
  $set = [Collections.Generic.HashSet[string]]::new()
  [void]$set.Add($t)
  foreach ($d in $graph[$t].deps) { foreach ($x in (Get-Closure $d)) { [void]$set.Add($x) } }
  $closure[$t] = $set
  return $set
}
$runnable = @($graph.Keys | Where-Object { $graph[$_].kind -in 'test', 'executable' } | Sort-Object)
$benches = @($graph.Keys | Where-Object { $graph[$_].kind -eq 'bench' } | Sort-Object)

# Longest source-directory prefix wins (tools/pqc vs tools, src/ui/scenes vs src/ui).
function Get-Owners([string] $file, [string[]] $kinds) {
  $best = 0; $owners = @()
  foreach ($t in $graph.Keys) {
    if ($graph[$t].kind -notin $kinds) { continue }
    foreach ($d in $graph[$t].dirs) {
      if ($file.StartsWith("$d/", [StringComparison]::OrdinalIgnoreCase)) {
        if ($d.Length -gt $best) { $best = $d.Length; $owners = @($t) }
        elseif ($d.Length -eq $best -and $owners -notcontains $t) { $owners += $t }
      }
    }
  }
  return $owners
}
function Get-Dependents([string] $module) {
  return @($runnable | Where-Object { (Get-Closure $_).Contains($module) })
}

$all = $false
$run = [Collections.Generic.SortedSet[string]]::new()      # executables whose tests run
$build = [Collections.Generic.SortedSet[string]]::new()    # extra targets built without running tests
$stdlibReaders = $null

foreach ($f in $Files) {
  $why = ''
  $lower = $f.ToLowerInvariant()
  if ($lower -match '\.md$' -or $lower -match '^(docs|reference|\.claude)/' -or $lower -match '^(license|\.gitignore|\.gitattributes)' -or
      $lower -match '^scripts/(get-sccache|clean-sccache)\.ps1$' -or $lower -match '^tools/(fixtures|fit|size|theme)/') {
    Say "  $f -> nothing (not part of the build or the tests)"; continue
  }
  if ($lower -match '^(cmakelists\.txt|cmakepresets\.json|cmake/|src/cmakelists\.txt|src/core/|tests/cmakelists\.txt|tests/[^/]+\.(cpp|h)$|tests/.*\.cmake$|scripts/(build|vsenv|affected)|scripts/ninjadeps\.cs)') {
    Say "  $f -> EVERYTHING (build infrastructure or core)"; $all = $true; continue
  }
  if ($lower -match '^stdlib/') {
    if ($null -eq $stdlibReaders) {
      $stdlibReaders = @($runnable | Where-Object { $graph[$_].kind -eq 'test' } | Where-Object {
          $t = $_
          @($graph[$t].dirs | Where-Object { Test-Path (Join-Path $root $_) } | ForEach-Object {
              Get-ChildItem -Path (Join-Path (Join-Path $root $_) '*') -File -Include *.cpp, *.h -ErrorAction SilentlyContinue |
                Select-String -Pattern '"stdlib"|/stdlib|stdlib(Dir|Path)\(|stdlib_(harness|reference|test)' -List }).Count -gt 0 }) + @($runnable | Where-Object { $graph[$_].kind -eq 'executable' -and (Get-Closure $_).Contains('pq_app') })
    }
    $stdlibReaders | ForEach-Object { [void]$run.Add($_) }
    Say "  $f -> stdlib readers: $($stdlibReaders -join ', ')"; continue
  }
  if ($lower -match '^assets/') {
    $deps = Get-Dependents 'pq_ui'; $deps | ForEach-Object { [void]$run.Add($_) }
    Say "  $f -> assets: dependents of pq_ui: $($deps -join ', ')"; continue
  }
  if ($lower -match '^bench/') {
    $owners = Get-Owners $f @('bench')
    if (-not $owners) { $owners = $benches }
    $owners | ForEach-Object { [void]$build.Add($_) }
    Say "  $f -> build $($owners -join ', ') (no tests)"; continue
  }
  if ($lower -match '^tools/bench/') { [void]$build.Add('pq_tools_append_results'); Say "  $f -> pq_tools_append_results"; continue }
  if ($lower -match '^tests/') {
    # A test source file: the executables that compile it; anything else (helpers, fixtures, goldens): every
    # executable built from that directory.
    $owners = @($graph.Keys | Where-Object { $graph[$_].kind -eq 'test' -and @($graph[$_].sources) -contains $f })
    if (-not $owners) { $owners = Get-Owners $f @('test') }
    if (-not $owners) { Say "  $f -> EVERYTHING (no test executable owns it)"; $all = $true; continue }
    $owners | ForEach-Object { [void]$run.Add($_) }
    Say "  $f -> $($owners -join ', ')"; continue
  }
  if ($lower -match '^(src|tools)/') {
    $owners = @(Get-Owners $f @('module', 'executable'))
    if (-not $owners) { Say "  $f -> EVERYTHING (no module owns it)"; $all = $true; continue }
    foreach ($o in $owners) {
      $deps = @(Get-Dependents $o)
      if ($o -eq 'pq_dsp' -and $lower -match '^src/dsp/(src/(kernels_[^/]*|tables\.cpp|simd\.h)|include/pq/dsp/(kernel|tables|atom_state)\.h)$') {
        $audio = @($deps | Where-Object { (Get-Closure $_).Contains('pq_engine') -or $_ -in 'pq_tests_dsp', 'pq_tests_graph' })
        $audio | ForEach-Object { [void]$run.Add($_) }
        if ($lower -match '/include/') { $deps | Where-Object { $audio -notcontains $_ } | ForEach-Object { [void]$build.Add($_) } }
        Say "  $f -> dsp kernels: run $($audio -join ', ')"
      } else {
        $deps | ForEach-Object { [void]$run.Add($_) }
        Say "  $f -> dependents of ${o}: $($deps -join ', ')"
      }
    }
    continue
  }
  Say "  $f -> EVERYTHING (unclassified path)"; $all = $true
}

# --- result ---------------------------------------------------------------------------------------------------
$tests = @($run | Where-Object { $graph.ContainsKey($_) })
$patterns = @()
$testExes = @($tests | Where-Object { $graph[$_].kind -eq 'test' })
if ($testExes) { $patterns += "($($testExes -join '|'))(_.*)?" }
if ($tests -contains 'pqdaw') { $patterns += 'pqdaw_.*' }
if ($build -contains 'pq_tools_append_results') { $patterns += 'pq_tools_append_results' }
if ($build -contains 'pq_bench_dsp') { $patterns += 'pq_bench_runner_smoke' }
# Only executables with CTest entries are worth building for their tests (pqc is built by the tests that need it).
$targets = @($tests | Where-Object { $graph[$_].kind -eq 'test' -or $_ -eq 'pqdaw' }) +
           @($build | Where-Object { $graph.ContainsKey($_) -and $graph[$_].kind -in 'test', 'bench' -or $_ -eq 'pqdaw' })
$targets = @($targets | Sort-Object -Unique)
$result = [pscustomobject]@{
  All       = $all
  Files     = $Files
  Targets   = $(if ($all) { @() } else { $targets })
  TestRegex = $(if ($all -or -not $patterns) { '' } else { '^(' + ($patterns -join '|') + ')$' })
}
if ($all) { Say 'affected: EVERYTHING (whole suite, default + slow)' }
elseif (-not $result.Targets -and -not $result.TestRegex) { Say 'affected: nothing to build or test' }
else {
  Say "affected: build $($result.Targets -join ' ')"
  Say "affected: tests $(if ($result.TestRegex) { $result.TestRegex } else { '(none)' })"
}
return $result
