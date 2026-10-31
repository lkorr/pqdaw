# M4 review: compiles probe_doc.cpp against the release pq_doc and its dependencies (run after a release build).
# Usage: pwsh build_probe.ps1 <repo root> <output dir>
param([string]$Root, [string]$Out)
. (Join-Path $Root 'scripts/vsenv.ps1')
$b = Join-Path $Root 'build/release'
$inc = @('src/document/include', 'src/tuning/include', 'src/core/include', 'src/graph/include', 'src/dsp/include') |
  ForEach-Object { '/I' + (Join-Path $Root $_) }
$inc += '/I' + (Join-Path $b '_deps/nlohmann_json-src/include')
$libs = @('src/document/pq_doc.lib', 'src/graph/pq_graph.lib', 'src/tuning/pq_tuning.lib', 'src/dsp/pq_dsp.lib',
          'src/core/pq_core.lib') | ForEach-Object { Join-Path $b $_ }
New-Item -ItemType Directory -Force $Out | Out-Null
Push-Location $Out
cl /nologo /TP /std:c++20 /MT /O2 /EHsc /utf-8 /arch:AVX2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Zc:__cplusplus /Zc:preprocessor `
  @inc (Join-Path $PSScriptRoot 'probe_doc.cpp') /Fe:probe_doc.exe /link @libs
Pop-Location
