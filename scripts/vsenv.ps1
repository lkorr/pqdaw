# Dot-source to enter the Visual Studio x64 developer environment (cl.exe, link.exe, dumpbin on PATH).
# Idempotent: does nothing if cl.exe is already available.
if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$env:PATH = "$installer;$env:PATH"   # VsDevCmd itself calls vswhere
$vs = & (Join-Path $installer 'vswhere.exe') -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 toolset was not found' }

Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
