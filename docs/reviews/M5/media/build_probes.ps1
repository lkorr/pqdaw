# M5 review (media): compile the standalone probe against the release pq_media / pq_dsp / pq_core libs (no cmake).
#   pwsh docs/reviews/M5/media/build_probes.ps1 [Name] [outDir]; then run "$outDir\probe_media.exe poll|collide|pqwf|xing"
param([string]$Name = 'probe_media', [string]$OutDir = "$env:TEMP\pq_review_m5")
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$inc = @('src\media\include', 'src\core\include', 'src\dsp\include') | ForEach-Object { "/I$root\$_" }
$codecs = Get-ChildItem "$root\build\release\src\media\CMakeFiles\pq_media_codecs.dir" -Recurse -Filter *.obj | ForEach-Object { $_.FullName }
$libs = @("$root\build\release\src\media\pq_media.lib", "$root\build\release\src\dsp\pq_dsp.lib",
          "$root\build\release\src\core\pq_core.lib") + $codecs
cl /nologo /TP /EHsc /O2 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- @inc `
  "$PSScriptRoot\$Name.cpp" /Fo"$OutDir\$Name.obj" /Fe"$OutDir\$Name.exe" /link @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
