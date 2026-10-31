<#
.SYNOPSIS
  Download the pinned sccache release (compiler cache, Apache-2.0) into %LOCALAPPDATA%\pqdaw\tools.

.DESCRIPTION
  Optional: builds work without it. When the binary is present, CMake uses it as the compiler launcher
  (cmake/PqCompilerCache.cmake) and scripts/build.ps1 prints its cache statistics. One cache directory,
  %LOCALAPPDATA%\pqdaw\sccache-shared, is shared by every checkout and worktree (scripts/build.ps1). See docs/DEPENDENCIES.md.

  The archive is checked against the SHA-256 pinned below before anything is extracted. Re-running is a
  no-op when the binary already exists. Existing build trees pick sccache up after `build.ps1 -Reconfigure`.

.EXAMPLE
  ./scripts/get-sccache.ps1
#>
$ErrorActionPreference = 'Stop'

$version = 'v0.17.0'
$asset = "sccache-$version-x86_64-pc-windows-msvc"
$url = "https://github.com/mozilla/sccache/releases/download/$version/$asset.zip"
$sha256 = 'e94cfc5b58cbe439302f586c1d1bd7980c2cd371d47bdf385ade657411e6f3ac'

# Fixed location (no version in the path): cmake/PqCompilerCache.cmake and scripts/build.ps1 look here.
$tools = Join-Path $env:LOCALAPPDATA 'pqdaw\tools'
$dest = Join-Path $tools 'sccache'
$exe = Join-Path $dest 'sccache.exe'
if ((Test-Path $exe) -and ((& $exe --version) -eq "sccache $($version.TrimStart('v'))")) {
  Write-Host "sccache $version already installed: $exe"
  return
}

New-Item -ItemType Directory -Force $tools | Out-Null
# Download and extract under unique temporary names, then rename into place: two concurrent runs cannot
# see each other's half-written files.
$tag = [guid]::NewGuid().ToString('N')
$zip = Join-Path $tools "$asset.$tag.zip"
$tmp = Join-Path $tools "$asset.$tag.tmp"
try {
  Write-Host "Downloading $url"
  Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
  $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
  if ($actual -ne $sha256) { throw "sccache archive SHA-256 mismatch: expected $sha256, got $actual" }
  Expand-Archive -Path $zip -DestinationPath $tmp
  $inner = Join-Path $tmp $asset
  if (-not (Test-Path (Join-Path $inner 'sccache.exe'))) { throw "unexpected archive layout: $inner\sccache.exe missing" }
  if (Test-Path $dest) {
    # A different (older) pin: move it aside first. Fails while one of its servers is running
    # (`$exe --stop-server` for each worktree's port, or wait for their idle timeout).
    Move-Item $dest (Join-Path $tools "sccache.old.$tag")
  }
  try {
    Move-Item $inner $dest
  } catch {
    if (-not (Test-Path $exe)) { throw }  # lost a race with a concurrent run: theirs is identical
  }
  Write-Host "sccache $version installed: $exe"
} finally {
  Remove-Item -Force -ErrorAction SilentlyContinue $zip
  Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tmp
}
