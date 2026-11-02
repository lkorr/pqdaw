# MM review (link / threads / lifetimes): compile a standalone probe against the check-preset libs (no cmake). Usage:
#   pwsh docs/reviews/MM/link/build_probes.ps1 probe_link [outDir] [args...]
#   pwsh docs/reviews/MM/link/build_probes.ps1 probe_gram
# Flags match build\check\compile_commands.json (/O2, asserts on, PQ_OPTIMIZED=1); the link line is pq_tests_app's.
param([string]$Name = 'probe_link', [string]$OutDir = "$env:TEMP\pq_review_mm\link", [string[]]$RunArgs = @())
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$b = "$root\build\check"
$inc = @('src\core\include', 'src\app\include', 'src\ui\include', 'src\ui\scenes\include', 'src\views\include', 'src\document\include',
  'src\tuning\include', 'src\graph\include', 'src\dsp\include', 'src\meta\include', 'src\media\include', 'src\library\include',
  'src\engine\include', 'src\compile\include', 'src\jit\include', 'cmake') | ForEach-Object { "/I$root\$_" }
$inc += @("/I$b\_deps\nlohmann_json-src\include", '/IC:\VulkanSDK\1.4.357.0\Include')
$llvm = 'OrcJIT ExecutionEngine JITLink OrcTargetProcess OrcShared WindowsDriver Option RuntimeDyld Passes Coroutines HipStdPar ipo FrontendOpenMP FrontendOffloading ObjectYAML FrontendAtomic FrontendDirective Linker Vectorize SandboxIR X86CodeGen CFGuard GlobalISel IRPrinter Instrumentation AsmPrinter SelectionDAG CodeGen Target ObjCARCOpts ScalarOpts AggressiveInstCombine InstCombine TransformUtils CGData BitWriter Analysis FrontendHLSL ProfileData Symbolize DebugInfoGSYM DebugInfoPDB DebugInfoCodeView DebugInfoMSF DebugInfoBTF DebugInfoDWARF Object TextAPI IRReader BitReader AsmParser Core Remarks BitstreamReader X86AsmParser MCParser X86Desc CodeGenTypes X86Disassembler MCDisassembler X86Info MC DebugInfoDWARFLowLevel BinaryFormat TargetParser Support Demangle'.Split(' ') | ForEach-Object { "C:\dev\llvm-23.1.3\lib\LLVM$_.lib" }
$libs = @('src\app\pq_app.lib', 'src\ui\pq_ui.lib', 'pq_build_paths.lib', 'src\ui\scenes\pq_ui_scenes.lib', 'src\views\pq_views.lib',
  'pq_harfbuzz.lib', '_deps\freetype-build\freetype.lib', 'src\library\pq_library.lib', 'src\document\pq_doc.lib', 'src\compile\pq_compile.lib',
  'src\meta\pq_meta.lib', 'src\jit\pq_jit.lib', 'src\engine\pq_engine.lib', 'src\tuning\pq_tuning.lib', 'src\media\pq_media.lib',
  '_deps\rtaudio-build\rtaudio.lib', 'src\graph\pq_graph.lib', 'src\dsp\pq_dsp.lib', 'src\core\pq_core.lib', '_deps\zlib-build\zlibstatic.lib',
  '_deps\zstd-build\lib\zstd_static.lib', '_deps\sdl3-build\SDL3-static.lib') | ForEach-Object { "$b\$_" }
$codecs = Get-ChildItem "$b\src\media\CMakeFiles\pq_media_codecs.dir" -Recurse -Filter *.obj -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
$cpu = Get-ChildItem "$b\src\core\CMakeFiles\pq_cpu_baseline.dir" -Recurse -Filter *.obj -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
$sys = 'C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib dwmapi.lib ksuser.lib mfplat.lib mfuuid.lib wmcodecdspuuid.lib avrt.lib Dbghelp.lib psapi.lib ws2_32.lib ntdll.lib delayimp.lib kernel32.lib user32.lib gdi32.lib winmm.lib imm32.lib oleaut32.lib version.lib uuid.lib advapi32.lib setupapi.lib ole32.lib shell32.lib winspool.lib comdlg32.lib'.Split(' ')
$rsp = "$OutDir\$Name.link.rsp"
(@('/link') + @($libs) + $llvm + $codecs + $cpu + $sys + @('/machine:x64', '/OPT:REF', '/subsystem:console', '-delayload:shell32.dll', '-delayload:ole32.dll', '-INCLUDE:malloc')) |
  ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } } | Join-String -Separator ' ' | Set-Content $rsp  # one line: cl's /link applies to its line
cl /nologo /TP /DNOMINMAX /DPQ_OPTIMIZED=1 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /Oi /Ot `
  -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- /Zc:__cplusplus /Zc:preprocessor /wd4324 @inc `
  "$PSScriptRoot\$Name.cpp" /Fo"$OutDir\$Name.obj" /Fe"$OutDir\$Name.exe" "@$rsp"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& "$OutDir\$Name.exe" @RunArgs
exit $LASTEXITCODE
