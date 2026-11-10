# M6 review (MIDI / live input, TU.1): compile a standalone probe against the release libs (no cmake). Usage:
#   pwsh docs/reviews/M6/input/build_probes.ps1 probe_input_ids [outDir]
#   pwsh docs/reviews/M6/input/build_probes.ps1 probe_tuning_window [outDir]
param([string]$Name = 'probe_input_ids', [string]$OutDir = "$env:TEMP\pq_review_m6_input")
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$b = "$root\build\release"
$inc = @('src\core\include', 'src\dsp\include', 'src\media\include', 'src\engine\include', 'src\graph\include', 'src\tuning\include',
  'src\document\include', 'src\meta\include') | ForEach-Object { "/I$root\$_" }
$inc += "/I$b\_deps\nlohmann_json-src\include"
$libs = @("$b\src\document\pq_doc.lib", "$b\src\meta\pq_meta.lib", "$b\src\graph\pq_graph.lib", "$b\src\engine\pq_engine.lib",
  "$b\src\media\pq_media.lib", "$b\src\tuning\pq_tuning.lib", "$b\src\dsp\pq_dsp.lib", "$b\src\core\pq_core.lib",
  "$b\_deps\rtaudio-build\rtaudio.lib") + @('ole32.lib', 'avrt.lib', 'advapi32.lib', 'user32.lib', 'winmm.lib', 'ksuser.lib',
  'psapi.lib', 'mfplat.lib', 'mfuuid.lib', 'wmcodecdspuuid.lib')
cl /nologo /TP /EHsc /O2 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DPQ_OPTIMIZED=1 -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- @inc `
  "$PSScriptRoot\$Name.cpp" /Fo"$OutDir\$Name.obj" /Fe"$OutDir\$Name.exe" /link @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& "$OutDir\$Name.exe"
