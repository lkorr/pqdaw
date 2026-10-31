# M5 review (engine audio): compile a standalone probe against the release libs (no cmake). Usage:
#   pwsh docs/reviews/M5/engine/build_probes.ps1 probe_preview_race [outDir]
param([string]$Name = 'probe_preview_race', [string]$OutDir = "$env:TEMP\pq_review_m5")
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$b = "$root\build\release"
$inc = @('src\core\include', 'src\dsp\include', 'src\media\include', 'src\engine\include', 'src\graph\include', 'src\tuning\include') | ForEach-Object { "/I$root\$_" }
$codecs = Get-ChildItem "$b\src\media\CMakeFiles\pq_media_codecs.dir" -Recurse -Filter *.obj | ForEach-Object { $_.FullName }
$libs = @("$b\src\engine\pq_engine.lib", "$b\src\media\pq_media.lib", "$b\src\graph\pq_graph.lib", "$b\src\tuning\pq_tuning.lib",
  "$b\src\dsp\pq_dsp.lib", "$b\src\core\pq_core.lib", "$b\_deps\rtaudio-build\rtaudio.lib") + $codecs + @('ole32.lib', 'avrt.lib', 'advapi32.lib', 'user32.lib', 'winmm.lib', 'ksuser.lib', 'mfplat.lib', 'mfuuid.lib', 'wmcodecdspuuid.lib')
cl /nologo /TP /EHsc /O2 /DNDEBUG /DNOMINMAX -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- @inc `
  "$PSScriptRoot\$Name.cpp" /Fo"$OutDir\$Name.obj" /Fe"$OutDir\$Name.exe" /link @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& "$OutDir\$Name.exe"
