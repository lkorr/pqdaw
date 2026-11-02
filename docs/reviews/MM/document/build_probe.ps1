# MM review (document): compiles probe_mm.cpp against the check-preset pq_doc and its dependencies (no cmake; the
# defines and flags of build/check/compile_commands.json). Usage: pwsh build_probe.ps1 <repo root> <output dir>
param([string]$Root, [string]$Out)
. (Join-Path $Root 'scripts/vsenv.ps1')
$b = Join-Path $Root 'build/check'
$inc = @('src/document/include', 'src/tuning/include', 'src/core/include', 'src/graph/include', 'src/dsp/include',
         'src/meta/include') | ForEach-Object { '/I' + (Join-Path $Root $_) }
$inc += '/I' + (Join-Path $b '_deps/nlohmann_json-src/include')
$libs = @('src/document/pq_doc.lib', 'src/meta/pq_meta.lib', 'src/graph/pq_graph.lib', 'src/tuning/pq_tuning.lib',
          'src/dsp/pq_dsp.lib', 'src/core/pq_core.lib') | ForEach-Object { Join-Path $b $_ }
New-Item -ItemType Directory -Force $Out | Out-Null
Push-Location $Out
cl /nologo /TP /std:c++20 /MT /O2 /EHsc /utf-8 /arch:AVX2 /fp:precise /permissive- /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
  /DPQ_OPTIMIZED=1 /D_CRT_SECURE_NO_WARNINGS /Zc:__cplusplus /Zc:preprocessor /wd4324 `
  @inc (Join-Path $PSScriptRoot 'probe_mm.cpp') /Fe:probe_mm.exe /link @libs
$code = $LASTEXITCODE
Pop-Location
exit $code
