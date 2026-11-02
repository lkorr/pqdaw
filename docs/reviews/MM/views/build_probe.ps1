# MM review (views): compile probe_views.cpp against the check libs (no cmake) and run it. Usage:
#   pwsh docs/reviews/MM/views/build_probe.ps1 [outDir] [probe letters...]
param([string]$OutDir = "$env:TEMP\pq_review_mm\views", [Parameter(ValueFromRemainingArguments)] [string[]]$Probes)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..\..\..").Path
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$b = "$root\build\check"
$inc = @('src\core\include','src\views\include','src\ui\include','src\document\include','src\tuning\include','src\graph\include',
         'src\dsp\include','src\meta\include','src\media\include','src\library\include','cmake') | ForEach-Object { "/I$root\$_" }
$inc += "/I$b\_deps\nlohmann_json-src\include"
$inc += "/IC:\VulkanSDK\1.4.357.0\Include"
$libs = @("$b\src\views\pq_views.lib","$b\pq_build_paths.lib","$b\src\ui\pq_ui.lib","C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib",
          "$b\_deps\sdl3-build\SDL3-static.lib","$b\_deps\zlib-build\zlibstatic.lib","$b\pq_harfbuzz.lib","$b\_deps\freetype-build\freetype.lib",
          "$b\src\library\pq_library.lib","$b\src\document\pq_doc.lib","$b\src\meta\pq_meta.lib","$b\src\graph\pq_graph.lib","$b\src\tuning\pq_tuning.lib",
          "$b\src\media\pq_media.lib","$b\src\dsp\pq_dsp.lib","$b\src\core\pq_core.lib",
          'kernel32.lib','user32.lib','gdi32.lib','winmm.lib','imm32.lib','oleaut32.lib','version.lib','uuid.lib','advapi32.lib','setupapi.lib',
          'dwmapi.lib','Dbghelp.lib','shell32.lib','ole32.lib','winspool.lib','comdlg32.lib')
# the check preset's flags (build\check\compile_commands.json): /O2, asserts on, PQ_OPTIMIZED
cl /nologo /TP /EHsc /O2 /Ob2 /Oi /Ot /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS /DPQ_OPTIMIZED=1 `
  -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- /Zc:__cplusplus /Zc:preprocessor /wd4324 /Z7 `
  /FIpq_build_paths.h @inc "$PSScriptRoot\probe_views.cpp" /Fo"$OutDir\probe_views.obj" /Fe"$OutDir\probe_views.exe" /link /DEBUG /subsystem:console @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& "$OutDir\probe_views.exe" $root @Probes
