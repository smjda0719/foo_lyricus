# 回滚到 2026-09-23 之前那个能正常工作的版本。
# 用法：先完全退出 foobar2000，再运行本脚本，然后手动启动播放器。
$ErrorActionPreference = 'Stop'
$dir  = Join-Path $env:APPDATA 'foobar2000-v2\user-components-x64\foo_lyricus'
$dst  = Join-Path $dir 'foo_lyricus.dll'
$src  = Join-Path $PSScriptRoot 'foo_lyricus.591360.knowngood.dll'

if (Get-Process foobar2000 -ErrorAction SilentlyContinue) {
    Write-Host 'foobar2000 还在运行 —— 先完全退出再跑这个脚本。' -ForegroundColor Red
    exit 1
}
Copy-Item -LiteralPath $src -Destination $dst -Force
Write-Host ("已回滚: {0:N0} B  {1}" -f (Get-Item $dst).Length, (Get-FileHash $dst -Algorithm SHA256).Hash) -ForegroundColor Green
