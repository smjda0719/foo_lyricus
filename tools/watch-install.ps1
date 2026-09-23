<#
  等待 foobar2000 退出 -> 自动安装新编译的组件 -> 可选自动重新启动。

  为什么需要它：
    组件 DLL 一旦被 foobar2000 加载就会被文件锁锁住，既不能覆盖也不能改名。
    所以每次迭代的固定动作是「关播放器 -> 拷贝 -> 开播放器」。
    这个脚本把「等它退出 + 拷贝 + 重启」自动化，用户只需要关一次播放器，
    不必等对面回话。

  用法：
    .\watch-install.ps1 -Relaunch
    .\watch-install.ps1 -WaitSeconds 120
    .\watch-install.ps1 -Platform Win32
#>

[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')] [string]$Platform = 'x64',
    [string]$ProcessName = 'foobar2000',
    [string]$ExePath = 'C:\Program Files\foobar2000\foobar2000.exe',
    [int]$WaitSeconds = 1800,
    [switch]$Relaunch
)

$ErrorActionPreference = 'Continue'

$root  = Split-Path -Parent $PSScriptRoot
$src   = Join-Path $root "bin\$Platform\Release\foo_lyricus.dll"
$arch  = if ($Platform -eq 'Win32') { 'user-components' } else { 'user-components-x64' }
$destD = Join-Path $env:APPDATA "foobar2000-v2\$arch\foo_lyricus"
$dest  = Join-Path $destD 'foo_lyricus.dll'

function Say($msg, $color = 'Gray') { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $msg) -ForegroundColor $color }

# ---- 0. 单实例保护 ---------------------------------------------------------
# 实测踩过：启动新的守候进程时忘了清理旧的，两个进程同时被 foobar2000 退出唤醒，
# 抢着拷贝同一个目标文件 —— 后到的那个 20 次重试全部失败（另一方已经把播放器
# 拉起来、DLL 又被锁住），日志里刷出一屏莫名其妙的错误。
$lockFile = Join-Path $env:TEMP 'lyricus-watch-install.lock'
if (Test-Path $lockFile) {
    $oldPid = (Get-Content $lockFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($oldPid -and (Get-Process -Id $oldPid -ErrorAction SilentlyContinue)) {
        Say "已有守候进程在运行（PID $oldPid），本次退出。" Yellow
        Say "如需替换：Stop-Process -Id $oldPid" Yellow
        exit 10
    }
    Say "发现陈旧锁文件（PID $oldPid 已不存在），继续。" DarkGray
}
$PID | Out-File -FilePath $lockFile -Encoding ascii -Force

if (-not (Test-Path $src)) { Say "找不到新编译的 DLL：$src" Red; exit 1 }
$srcInfo = Get-Item $src
Say ("待安装: {0}  ({1:N0} B, {2:HH:mm:ss})" -f $srcInfo.Name, $srcInfo.Length, $srcInfo.LastWriteTime) Cyan

# ---- 1. 等到进程退出 --------------------------------------------------------
if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
    Say "$ProcessName 正在运行，等待它退出（最多 $WaitSeconds 秒）..."
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 700
    }
    if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
        Say "等待超时，$ProcessName 仍在运行，放弃。" Red
        exit 2
    }
    Say "$ProcessName 已退出" Green
} else {
    Say "$ProcessName 本来就没运行，直接安装"
}

# ---- 2. 拷贝（带重试，刚退出时句柄可能还没释放）-----------------------------
New-Item -ItemType Directory -Force -Path $destD | Out-Null

$copied = $false
for ($i = 1; $i -le 20; $i++) {
    try {
        Copy-Item -LiteralPath $src -Destination $dest -Force -ErrorAction Stop
        $copied = $true
        break
    } catch {
        Say ("  第 {0} 次拷贝失败：{1}" -f $i, $_.Exception.Message.Trim()) Yellow
        Start-Sleep -Milliseconds 500
    }
}

if (-not $copied) { Say "拷贝失败，放弃。可能句柄仍未释放。" Red; exit 3 }

$h1 = (Get-FileHash -LiteralPath $src  -Algorithm SHA256).Hash
$h2 = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
Say ("已安装: {0}" -f $dest) Green
Say ("  大小 {0:N0} B   哈希一致 {1}" -f (Get-Item $dest).Length, ($h1 -eq $h2)) Green

# ---- 2.5 顺带部署 SVG 图标 --------------------------------------------------
# 运行期从 DLL 同级的 resources\ 目录加载，所以必须一起拷过去。
$resSrc = Join-Path $root 'resources'
if (Test-Path $resSrc) {
    $resDst = Join-Path $destD 'resources'
    New-Item -ItemType Directory -Force -Path $resDst | Out-Null
    Copy-Item (Join-Path $resSrc '*.svg') $resDst -Force -ErrorAction SilentlyContinue
    $n = (Get-ChildItem $resDst -Filter *.svg -ErrorAction SilentlyContinue | Measure-Object).Count
    Say ("  已部署 {0} 个 SVG 图标" -f $n) Green
}

# ---- 3. 可选重新启动 --------------------------------------------------------
if ($Relaunch) {
    if (Test-Path $ExePath) {
        Start-Process -FilePath $ExePath
        Say "已重新启动 foobar2000" Green
    } else {
        Say "找不到 $ExePath，请手动启动" Yellow
    }
}

# 释放单实例锁
Remove-Item $lockFile -Force -ErrorAction SilentlyContinue
