<#
.SYNOPSIS
  Deletes per-checkout compiler-cache directories whose checkout no longer exists (removed worktrees), and the
  pre-v3 shared cache directory. Each cache directory records its checkout path in checkout.txt (scripts/build.ps1).
#>
$ErrorActionPreference = 'Stop'
$root = if ($env:PQ_SCCACHE_DIR) { $env:PQ_SCCACHE_DIR } else { Join-Path $env:LOCALAPPDATA 'pqdaw\sccache-v3' }
$old = Join-Path $env:LOCALAPPDATA 'pqdaw\sccache'
if (Test-Path $old) { Remove-Item -Recurse -Force $old; Write-Host "removed $old (pre-v3 shared cache)" }
if (-not (Test-Path $root)) { return }
foreach ($d in Get-ChildItem -Directory $root) {
  $marker = Join-Path $d.FullName 'checkout.txt'
  $checkout = if (Test-Path $marker) { Get-Content -Raw $marker } else { '' }
  if (-not $checkout -or -not (Test-Path $checkout)) {
    Remove-Item -Recurse -Force $d.FullName
    Write-Host "removed $($d.FullName) ($checkout)"
  }
}
