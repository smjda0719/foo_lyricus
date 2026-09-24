<#
  守候进程：盯着编译产物，一有新版本就自动热安装。

  为什么改成常驻
  --------------
  原来的流程是「等播放器退出 -> 拷贝 -> 重启」，而那个模式**已经过时了**：
  它存在的唯一理由是绕开 foobar2000「立即重启」那个约 1 秒的空档
  （旧轮询周期约 900ms，整个跳过去了，结果装了旧版还一声不吭）。
  但 hot-install 的改名方案**根本不依赖那个时间窗口** —— 什么时候跑都行。

  于是守候进程该盯的不是「播放器退出了没」，而是「编译产物变了没」。

  用法
  ----
    .\watch-install.ps1                 # 常驻；装完继续等，且**关掉播放器后自动拉起**
    .\watch-install.ps1 -Once           # 装一次就退出
    .\watch-install.ps1 -Relaunch       # 装完**立刻**重启（会打断播放）
    .\watch-install.ps1 -RelaunchOnExit:$false   # 关掉"退出后自动拉起"
    .\watch-install.ps1 -IntervalMs 500

  两条重启路径，别搞混
  --------------------
    -Relaunch          装完**立刻**重启 —— 会打断正在听的歌
    -RelaunchOnExit    装完**不打断**；等你下次关播放器时自动拉起来（默认开）
                       只在"装了新版、而播放器当时在运行"时才生效，
                       而且**只拉一次** —— 免得你想彻底关掉时跟它拉锯

  注意
  ----
  * 装上去之后，**运行中的播放器仍然执行内存里的旧代码**，新版要下次启动
    才生效 —— 这是 foobar2000 的模型，组件不能运行中重载。
    `-RelaunchOnExit` 就是把这个"下次启动"自动化掉。
  * 必须等编译**写完**再装。判断方式是「大小 + 修改时间连续两次采样都不变」，
    不能只看哈希变了就动手 —— 链接器写到一半时哈希也是"变了的"，
    那时候拷过去会装上一个半截的 DLL，而且**哈希校验查不出来**
    （源和目标是同一份半截）。
#>

[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')] [string]$Platform = 'x64',
    [string]$ProcessName = 'foobar2000',
    [string]$ExePath = 'C:\Program Files\foobar2000\foobar2000.exe',
    [int]$IntervalMs = 1000,
    [switch]$Relaunch,
    [bool]$RelaunchOnExit = $true,
    [switch]$Once
)

$ErrorActionPreference = 'Continue'

$root  = Split-Path -Parent $PSScriptRoot
$src   = Join-Path $root "bin\$Platform\Release\foo_lyricus.dll"
$arch  = if ($Platform -eq 'Win32') { 'user-components' } else { 'user-components-x64' }
$destD = Join-Path $env:APPDATA "foobar2000-v2\$arch\foo_lyricus"
$dest  = Join-Path $destD 'foo_lyricus.dll'
$hot   = Join-Path $PSScriptRoot 'hot-install.ps1'

$log = Join-Path $root 'build\watch-install.log'

# 同时写一份日志文件：常驻进程的输出事后要能查（Write-Host 不进程重定向，
# 所以不能指望 Start-Process -RedirectStandardOutput 把 -WindowStyle Hidden
# 的窗口内容捞出来）。
function Say($msg, $color = 'Gray') {
    $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $msg
    Write-Host $line -ForegroundColor $color
    try { Add-Content -LiteralPath $log -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
}

# ---- 单实例保护 -------------------------------------------------------------
# 两个守候进程同时装同一个文件会互相踩：一个刚把目标改名让路，
# 另一个正好在拷贝 —— 后到的会因为目标被占而失败，日志刷一屏莫名其妙的错误。
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

if (-not (Test-Path $src)) {
    Say "找不到编译产物：$src" Red
    Remove-Item $lockFile -Force -ErrorAction SilentlyContinue
    exit 1
}

Say "守候中：$src" Cyan
Say ("  轮询 {0} ms；目标 {1}" -f $IntervalMs, $dest) DarkGray
if ($Relaunch) { Say '  装完会自动重启播放器（会打断播放）' Yellow }

function Get-HashOrNull($p) {
    try {
        if (Test-Path -LiteralPath $p) {
            return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
        }
    } catch { }
    return $null
}

# 「已装版本」的判据取**目标文件**的哈希，而不是我们自己记的状态 ——
# 这样即使中途有别人（或手动）装过，也不会重复劳动。
$lastInstalled = Get-HashOrNull $dest
$installs = 0

# 上一次采样到的 (大小, 修改时间)，用来判断编译写完没有
$prevSig = $null

# 「新版已装好，但播放器还在跑旧代码」。
#
# 置位的时机：装完一次新构建、而播放器**当时在运行**（它加载不了新版，
# 得下次启动才生效）。
#
# 用途：一旦播放器退出，就把它拉起来 —— 这样用户只需要「关掉播放器」，
# 它自己带着新版回来，不必记得再手动开一次。
# 这是老脚本「等退出 -> 装 -> 重启」那条流程里真正有用的部分；
# 改成盯编译产物时被我弄丢过一次，用户提了才补回来。
#
# ⚠️ 拉起之后立刻清零：**只自动拉起一次**。
#    否则用户想彻底关掉播放器时，会跟守候进程来回拉锯。
$pendingNewVersion = $false

while ($true) {
    Start-Sleep -Milliseconds $IntervalMs

    # ---- a) 播放器退出了、且有新版在等 -> 拉起来 ----
    if ($RelaunchOnExit -and $pendingNewVersion) {
        if (-not (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)) {
            if (Test-Path $ExePath) {
                Start-Sleep -Milliseconds 1200   # 等它退干净，别和残留句柄抢
                Start-Process -FilePath $ExePath
                $pendingNewVersion = $false      # ★ 只拉一次，见上面的说明
                Say '播放器已退出，已自动拉起（新版生效）。' Green
            } else {
                Say "找不到 $ExePath，无法自动拉起" Yellow
                $pendingNewVersion = $false
            }
        }
    }

    if (-not (Test-Path -LiteralPath $src)) { continue }

    $fi = $null
    try { $fi = Get-Item -LiteralPath $src -ErrorAction Stop } catch { continue }
    $sig = "{0}:{1}" -f $fi.Length, $fi.LastWriteTimeUtc.Ticks

    if ($sig -ne $prevSig) {
        # 还在变 —— 编译没写完，下一轮再看
        $prevSig = $sig
        continue
    }

    # 连续两次采样一致 = 编译已经停笔，可以安全读取
    $srcHash = Get-HashOrNull $src
    if ($null -eq $srcHash) { continue }
    if ($srcHash -eq $lastInstalled) { continue }   # 没有新版本

    Say ("发现新构建 {0:N0} B  {1}" -f $fi.Length, $srcHash.Substring(0, 16)) Cyan

    & pwsh -NoProfile -File $hot -Platform $Platform 2>&1 |
        Where-Object { $_ -match '已安装|已把在用|哈希|失败|错误|不存在|回滚|播放器' } |
        ForEach-Object { Write-Host "    $_"; Add-Content -LiteralPath $log -Value ("    " + $_) -Encoding UTF8 -ErrorAction SilentlyContinue }

    $after = Get-HashOrNull $dest
    if ($after -eq $srcHash) {
        ++$installs
        $lastInstalled = $after

        # 播放器在运行时，它加载不了新版 —— 记下"有新版在等"，
        # 等它一退出就自动拉起来（见 $pendingNewVersion 的说明）。
        $playerRunning = [bool](Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
        if ($playerRunning -and $RelaunchOnExit) {
            $pendingNewVersion = $true
            Say "第 $installs 次安装完成。播放器仍在运行 —— 关掉它就会自动带着新版回来。" Green
        } else {
            Say "第 $installs 次安装完成。重启播放器后生效。" Green
        }

        if ($Relaunch -and (Test-Path $ExePath)) {
            Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
                Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 800
            Start-Process -FilePath $ExePath
            $pendingNewVersion = $false
            Say '已立刻重启播放器（新版已生效）' Green
        }
    } else {
        Say '安装后哈希不一致 —— 没装上，下一轮重试。' Red
    }

    if ($Once) { break }
}

Remove-Item $lockFile -Force -ErrorAction SilentlyContinue
