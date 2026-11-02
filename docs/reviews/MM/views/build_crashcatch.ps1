# MM review (views): builds crashcatch.exe (a minimal debugger for the GPU-golden SIGSEGVs). Usage:
#   pwsh docs/reviews/MM/views/build_crashcatch.ps1 [outDir]
param([string]$OutDir = "$env:TEMP\pq_review_mm\views")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..\..\..").Path
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
cl /nologo /EHsc /O1 /MT /W3 /DUNICODE /D_UNICODE "$PSScriptRoot\crashcatch.cpp" /Fo"$OutDir\crashcatch.obj" /Fe"$OutDir\crashcatch.exe" /link dbghelp.lib
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
