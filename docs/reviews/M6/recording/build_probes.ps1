# M6 review (recording): compile a standalone probe against the release libs (no cmake) and run it. Usage:
#   pwsh docs/reviews/M6/recording/build_probes.ps1 <probe> [outDir]
#   low-level probes (pq_media / pq_engine only): probe_wav_kill, probe_capture_drift
#   app-level probes (the whole link line of pq_tests_playback, read from build.ninja): probe_unsaved_delete,
#   probe_record_link (doctest, tests/app/playback_fixture.h)
param([string]$Name = 'probe_wav_kill', [string]$OutDir = "$env:TEMP\pq_review_m6_recording")
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\..\..\.."
. "$root\scripts\vsenv.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$b = "$root\build\release"
$inc = @('src\core\include', 'src\dsp\include', 'src\media\include', 'src\engine\include', 'src\graph\include', 'src\tuning\include',
  'src\document\include', 'src\views\include', 'src\ui\include', 'src\meta\include', 'src\library\include', 'src\app\include',
  'src\compile\include', 'src\jit\include', 'tests', 'tests\app') | ForEach-Object { "/I$root\$_" }
$inc += "/I$b\_deps\doctest-src"
$inc += Get-ChildItem "$b\_deps" -Directory -Filter '*-src' | ForEach-Object { "/I$($_.FullName)\include" }
$inc += Get-ChildItem "$b\_deps" -Directory -Filter '*-src' | ForEach-Object { "/I$($_.FullName)\single_include" }
$codecs = Get-ChildItem "$b\src\media\CMakeFiles\pq_media_codecs.dir" -Recurse -Filter *.obj | ForEach-Object { $_.FullName }
$app = @('probe_unsaved_delete', 'probe_record_link')
if ($app -contains $Name) {
  # The test executable's own link line: every module, LLVM, the system libraries.
  $ninja = Get-Content "$b\build.ninja" -Raw
  $m = [regex]::Match($ninja, 'build tests\\pq_tests_playback\.exe:[^\n]*\n(?:  [^\n]*\n)*?  LINK_LIBRARIES = ([^\n]*)')
  if (-not $m.Success) { throw "no link line for pq_tests_playback in build.ninja" }
  $libs = ($m.Groups[1].Value -split '\s+' | Where-Object { $_ -ne '' } | ForEach-Object {
      if ($_ -match '^[A-Za-z_]' -and $_ -match '\\' -and -not ($_ -match '^[A-Za-z]:')) { "$b\$_" } elseif ($_ -match '^pq_.*\.lib$') { "$b\$_" } else { $_ } })
  $libs += $codecs
} else {
  $libs = @("$b\src\engine\pq_engine.lib", "$b\src\media\pq_media.lib", "$b\src\graph\pq_graph.lib", "$b\src\tuning\pq_tuning.lib",
    "$b\src\dsp\pq_dsp.lib", "$b\src\core\pq_core.lib", "$b\_deps\rtaudio-build\rtaudio.lib", "$b\_deps\zstd-build\lib\zstd_static.lib",
    "$b\_deps\zlib-build\zlibstatic.lib") + $codecs + @('ole32.lib', 'avrt.lib', 'advapi32.lib', 'user32.lib', 'winmm.lib', 'ksuser.lib',
    'mfplat.lib', 'mfuuid.lib', 'wmcodecdspuuid.lib')
}
$inc += @("/I$root\cmake", "/I$root\src\ui\scenes\include", "/FIpq_build_paths.h")
cl /nologo /TP /EHsc /O2 /Ob2 /Oi /Ot /DNDEBUG /DNOMINMAX /DPQ_OPTIMIZED=1 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS `
  /DPQ_TEST_DATA_DIR=pq_build_test_data_dir -std:c++20 -MT /utf-8 /arch:AVX2 /fp:precise /permissive- /bigobj /Zc:preprocessor /Zc:__cplusplus /wd4324 @inc `
  "$PSScriptRoot\$Name.cpp" /Fo"$OutDir\$Name.obj" /Fe"$OutDir\$Name.exe" /link /IGNORE:4099 @libs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
& "$OutDir\$Name.exe"
