<#
.SYNOPSIS
  Compiler-cache housekeeping. Since T0.x-buildspeed2 every checkout shares one cache
  (%LOCALAPPDATA%\pqdaw\sccache-shared, 20 GB cap; sccache evicts the least recently used entries itself), so a
  removed worktree leaves nothing behind. This deletes the caches of earlier layouts (the per-checkout
  sccache-v3\<hash> directories and the pre-v3 sccache directory) and drops removed worktrees from the shared
  server's base-directory list. -All also empties the shared cache (stops the server first).
#>
param([switch] $All)
$ErrorActionPreference = 'Stop'
$base = Join-Path $env:LOCALAPPDATA 'pqdaw'
foreach ($old in @('sccache-v3', 'sccache')) {
  $p = Join-Path $base $old
  if (Test-Path $p) { Remove-Item -Recurse -Force $p; Write-Host "removed $p (per-checkout cache layout before T0.x-buildspeed2)" }
}
$shared = if ($env:PQ_SCCACHE_DIR) { $env:PQ_SCCACHE_DIR } else { Join-Path $base 'sccache-shared' }
$state = Join-Path $shared 'server-basedirs.txt'
if (Test-Path $state) {
  $keep = @(Get-Content $state | Where-Object { $_ -and (Test-Path $_) })
  Set-Content -Path $state -Value $keep   # the running server keeps its list until it restarts; harmless
}
if ($All -and (Test-Path $shared)) {
  $sccache = Join-Path $base 'tools\sccache\sccache.exe'
  if (Test-Path $sccache) { $env:SCCACHE_SERVER_PORT = '14299'; & $sccache --stop-server *> $null }
  Remove-Item -Recurse -Force $shared
  Write-Host "removed $shared"
}
