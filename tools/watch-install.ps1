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

# ---- 1+2. 等退出 + 拷贝，合成一个循环 ---------------------------------------
#
# 为什么不写成「等一次退出 → 拷一次」：
#   播放器会**快速重启** —— foobar2000 自己的「立即重启」（改 UI 设置时会弹）
#   只停机约 1 秒。我们探测到退出、正要拷贝时，新进程可能已经把 DLL 重新锁上了。
#   一次性逻辑此时只有两条烂路：
#     * 轮询太粗 -> 整个空档被跳过，守候进程一直傻等，新版**没装上还不报错**；
#     * 拷贝重试 20 次全失败 -> 直接 exit 3，同样没装上。
#     这两条都让「没装上」这件事不够显眼。
#
# 循环版：拷贝不成就回到循环顶，接着等**下一次**退出，直到整体超时。
#   成功的判据是**哈希一致**，不是「Copy-Item 没抛异常」——
#   文件被占用时 Copy-Item 会抛，但半途失败之类的情况要靠哈希才认得出。
#
# 轮询间隔 150ms 是拿事故换来的：原本 700ms，加上 Get-Process 自身的开销，
# 实际周期接近 900ms，1 秒的空档就这么被跳过去了。

New-Item -ItemType Directory -Force -Path $destD | Out-Null

$deadline  = (Get-Date).AddSeconds($WaitSeconds)
$installed = $false
$saidWaiting = $false

while ((Get-Date) -lt $deadline) {

    # a) 进程还在 -> 接着等。轮询必须密（见上面的说明）。
    if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
        if (-not $saidWaiting) {
            Say "$ProcessName 正在运行，等待它退出（最多 $WaitSeconds 秒）..."
            $saidWaiting = $true
        }
        Start-Sleep -Milliseconds 150
        continue
    }

    # b) 进程不在了 -> 试拷贝。刚退出时句柄可能还没释放，给几次机会。
    for ($i = 1; $i -le 12; $i++) {
        try {
            Copy-Item -LiteralPath $src -Destination $dest -Force -ErrorAction Stop
            $installed = $true
            break
        } catch {
            Start-Sleep -Milliseconds 250
        }
        # 重试期间播放器又起来了就别耗着，回循环顶等下一次退出
        if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) { break }
    }

    if ($installed) {
        Say "$ProcessName 已退出" Green
        break
    }
}

if (-not $installed) {
    Say "等待超时（$WaitSeconds 秒内没拿到可写的窗口），放弃。" Red
    Remove-Item $lockFile -Force -ErrorAction SilentlyContinue
    exit 2
}

# 复核：哈希不一致就等于没装上，不能只看 Copy-Item 有没有抛
$h1 = (Get-FileHash -LiteralPath $src  -Algorithm SHA256).Hash
$h2 = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
if ($h1 -ne $h2) {
    Say "拷贝后哈希不一致，安装视为失败。" Red
    Remove-Item $lockFile -Force -ErrorAction SilentlyContinue
    exit 3
}

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
