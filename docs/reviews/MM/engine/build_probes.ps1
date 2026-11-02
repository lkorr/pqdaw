# MM review (engine RT paths): compile a standalone probe against the check-preset libs (no cmake). Usage:
#   pwsh docs/reviews/MM/engine/build_probes.ps1 probe_pw_backward [outDir]
# Flags match build\check\compile_commands.json for pq_dsp (/O2, asserts on, PQ_OPTIMIZED=1, AVX2, /fp:precise).
param([string]$Name = 'probe_pw_backward', [string]$OutDir = "$env:TEMP\pq_review_mm\engine")
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$b = "$root\build\check"
$inc = @('src\core\include', 'src\dsp\include', 'src\engine\include', 'src\graph\include', 'src\tuning\include', 'tests\dsp', 'tests') |
  ForEach-Object { "/I$root\$_" }
$inc += "/I$b\_deps\doctest-src"
$libs = @("$b\src\engine\pq_engine.lib", "$b\src\graph\pq_graph.lib", "$b\src\tuning\pq_tuning.lib", "$b\src\dsp\pq_dsp.lib", "$b\src\core\pq_core.lib")
cl /nologo /TP /EHsc /O2 /Ob2 /Oi /Ot /DNOMINMAX /DPQ_OPTIMIZED=1 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DDOCTEST_CONFIG_DISABLE `
  -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- /Zc:__cplusplus /Zc:preprocessor @inc `
  "$PSScriptRoot\$Name.cpp" /Fo"$OutDir\$Name.obj" /Fe"$OutDir\$Name.exe" /link @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& "$OutDir\$Name.exe" @args
