# M5 review (library): compile the standalone fuzzer/probe against the release libs (no cmake). Usage:
#   pwsh docs/reviews/M5/library/build_probes.ps1 [outDir]
#   then: $env:TEMP\pq_review_m5\fuzz_library.exe fuzz $env:TEMP\pq_review_m5\work 20000 1   (see the .cpp for modes)
param([string]$OutDir = "$env:TEMP\pq_review_m5")
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$inc = @('src\library\include', 'src\core\include', 'src\media\include', 'src\document\include', 'src\tuning\include',
         'src\dsp\include', 'src\graph\include') | ForEach-Object { "/I$root\$_" }
$rel = "$root\build\release"
$libs = @("$rel\src\library\pq_library.lib", "$rel\src\media\pq_media.lib", "$rel\src\document\pq_doc.lib",
          "$rel\src\graph\pq_graph.lib", "$rel\src\tuning\pq_tuning.lib", "$rel\src\dsp\pq_dsp.lib",
          "$rel\src\core\pq_core.lib", "$rel\_deps\zstd-build\lib\zstd_static.lib", "$rel\_deps\zlib-build\zlibstatic.lib",
          'shell32.lib', 'ole32.lib', 'advapi32.lib', 'user32.lib')
cl /nologo /TP /EHsc /O2 /DNDEBUG /DNOMINMAX -std:c++20 -MT /utf-8 /arch:AVX2 /permissive- @inc `
  "$PSScriptRoot\fuzz_library.cpp" /Fo"$OutDir\fuzz_library.obj" /Fe"$OutDir\fuzz_library.exe" /link @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
