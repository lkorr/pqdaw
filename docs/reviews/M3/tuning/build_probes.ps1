# M3 review: compile a standalone probe against the release pq_tuning/pq_core libs (no cmake). Usage:
#   pwsh docs/reviews/M3/tuning/build_probes.ps1 probe_linear  [outDir]
param([string]$Name = 'probe_linear', [string]$OutDir = "$env:TEMP\pq_review_tuning")
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$inc = @('src\tuning\include', 'src\core\include', 'src\dsp\include') | ForEach-Object { "/I$root\$_" }
$libs = @("$root\build\release\src\tuning\pq_tuning.lib", "$root\build\release\src\core\pq_core.lib")
cl /nologo /TP /EHsc /O2 /DNDEBUG /DNOMINMAX -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- @inc `
  "$PSScriptRoot\$Name.cpp" /Fo"$OutDir\$Name.obj" /Fe"$OutDir\$Name.exe" /link @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& "$OutDir\$Name.exe"
