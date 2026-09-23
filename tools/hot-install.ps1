<#
  热安装：**不关播放器**就把新编译的组件换上去。

  原理
  ----
  Windows 不允许覆盖或删除一个已作为镜像加载的 DLL，但**允许给它改名** ——
  镜像节是以 FILE_SHARE_DELETE 打开的，目录项可以动，映射关系不受影响。
  于是「先把旧的改名让路，再把新的拷到原名」就能在播放器运行中完成安装。

  为什么需要它
  ------------
  原来的流程是「等播放器退出 -> 拷贝 -> 重启」。踩过的坑：foobar2000 自己的
  「立即重启」（改 UI 设置时会弹）只停机约 1 秒，而守候脚本的轮询周期
  （700ms + Get-Process 自身开销 ≈ 900ms）**整个跳过了那个空档** ——
  结果是播放器明明重启了，装的还是上一版，而且一声不吭，极难发现。

  热安装根本不依赖那个时间窗口：什么时候跑都行，跑几次都行。

  代价
  ----
  运行中的播放器仍然执行内存里的旧代码，**新版要下次启动才生效**。
  这不是缺陷 —— 组件本来就不能在运行中重载，这是 foobar2000 的模型。
  好处是：之后**任何一次**重启（包括它自己的快速重启）都会加载到新版。

  用法
  ----
    .\hot-install.ps1                 # 装 bin\x64\Release 的产物
    .\hot-install.ps1 -Platform Win32
    .\hot-install.ps1 -KeepBackup:$false   # 不留 .old（省得占地方）
    .\hot-install.ps1 -WhatIf              # 只看会做什么，不动文件
#>

[CmdletBinding()]
param(
    [ValidateSet('x64', 'Win32')] [string]$Platform = 'x64',
    [string]$ProcessName = 'foobar2000',
    [bool]$KeepBackup = $true,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$src   = Join-Path $root "bin\$Platform\Release\foo_lyricus.dll"
$arch  = if ($Platform -eq 'Win32') { 'user-components' } else { 'user-components-x64' }
$destD = Join-Path $env:APPDATA "foobar2000-v2\$arch\foo_lyricus"
$dest  = Join-Path $destD 'foo_lyricus.dll'

# 备份名**必须带时间戳**，不能用固定的 foo_lyricus.dll.old。
#
# 原因很微妙：上一次热安装把当时那份 DLL 改名成了 .old，而那个 .old
# **正是播放器此刻正在执行的镜像** —— Windows 对已加载镜像的改名是
# "文件对象跟着新名字走"，所以 .old 自己也被锁着，既不能删也不能覆盖。
# 用固定名字的话，这个脚本**第二次用就必然失败**
#（报「当文件已存在时，无法创建该文件」），而且看起来像是改名这条路不通。
#
# 后缀特意**不以 .dll 结尾**：foobar2000 只扫描 *.dll，不会去加载这些备份。
$old = Join-Path $destD ("foo_lyricus.{0}.old" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

function Say($m, $c = 'Gray') { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $m) -ForegroundColor $c }
function Hash($p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }

if (-not (Test-Path $src)) { Say "找不到编译产物：$src" Red; exit 1 }
New-Item -ItemType Directory -Force -Path $destD | Out-Null

$srcHash = Hash $src
$srcSize = (Get-Item $src).Length
Say ("源: {0:N0} B  {1}" -f $srcSize, $srcHash.Substring(0, 16)) Cyan

if (Test-Path $dest) {
    $destHash = Hash $dest
    if ($destHash -eq $srcHash) { Say '目标已是同一版本，无需安装。' Green; exit 0 }
    Say ("在装: {0:N0} B  {1}" -f (Get-Item $dest).Length, $destHash.Substring(0, 16))
} else {
    $destHash = $null
    Say '目标不存在，全新安装。'
}

$running = [bool](Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)

if ($WhatIf) {
    Say ("[WhatIf] 播放器运行中={0}；会改名让路 -> 拷入 -> 校验哈希" -f $running) Yellow
    exit 0
}

# ---- 1. 旧文件让路 ----------------------------------------------------------
# 三种情况：
#   * 目标不存在      -> 直接拷
#   * 播放器没在运行  -> 直接覆盖（没有锁）
#   * 播放器在运行    -> 必须靠"改名"绕过镜像锁
$movedAside = $false
if (Test-Path $dest) {
    if ($running) {
        try {
            Move-Item -LiteralPath $dest -Destination $old -Force
            $movedAside = $true
            Say '已把在用的旧版改名让路（镜像锁绕过了）' Green
        } catch {
            Say ("改名失败：{0}" -f $_.Exception.Message.Trim()) Red
            Say '=> 这条路在当前环境走不通，只能等播放器退出后再装。' Red
            exit 2
        }
    } else {
        # 没运行就直接删掉，省一次改名
        Remove-Item -LiteralPath $dest -Force
    }
}

# ---- 2. 拷入新版 ------------------------------------------------------------
try {
    Copy-Item -LiteralPath $src -Destination $dest -Force
} catch {
    Say ("拷贝失败：{0}" -f $_.Exception.Message.Trim()) Red
    if ($movedAside) {
        Remove-Item -LiteralPath $dest -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $old -Destination $dest -Force -ErrorAction SilentlyContinue
        Say '已回滚到旧版。' Yellow
    }
    exit 3
}

# ---- 3. 按哈希复核 ----------------------------------------------------------
# 不能只看"Copy-Item 没抛异常" —— 半途失败之类要靠哈希才认得出。
if ((Hash $dest) -ne $srcHash) {
    Say '拷入后哈希不一致，安装失败。' Red
    if ($movedAside) {
        Remove-Item -LiteralPath $dest -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $old -Destination $dest -Force -ErrorAction SilentlyContinue
        Say '已回滚到旧版。' Yellow
    }
    exit 4
}

Say ("已安装: {0}" -f $dest) Green
Say ("  {0:N0} B  {1}" -f $srcSize, $srcHash.Substring(0, 16)) Green

# ---- 4. 顺带部署 SVG 图标 ---------------------------------------------------
$resSrc = Join-Path $root 'resources'
if (Test-Path $resSrc) {
    $resDst = Join-Path $destD 'resources'
    New-Item -ItemType Directory -Force -Path $resDst | Out-Null
    Copy-Item (Join-Path $resSrc '*.svg') $resDst -Force -ErrorAction SilentlyContinue
    $n = (Get-ChildItem $resDst -Filter *.svg -ErrorAction SilentlyContinue | Measure-Object).Count
    Say ("  已部署 {0} 个 SVG 图标" -f $n) Green
}

# ---- 5. 备份去留 ------------------------------------------------------------
if ($movedAside) {
    if ($KeepBackup) {
        Say ("  旧版保留在 {0}" -f (Split-Path $old -Leaf)) DarkGray
        Say '  （要回滚就把它改回 foo_lyricus.dll —— 需先完全退出播放器）' DarkGray
    } else {
        Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
        Say '  已删除备份' DarkGray
    }
}

# 顺手清掉更早的备份。
# 当前正在被播放器执行的那一份**删不掉**（镜像锁），静默跳过就是了 ——
# 它会在播放器退出后的下一次运行里被清掉。
$stale = @()
$stale += Get-ChildItem $destD -Filter 'foo_lyricus.*.old' -File -ErrorAction SilentlyContinue
$stale += Get-ChildItem $destD -Filter 'foo_lyricus.dll.old' -File -ErrorAction SilentlyContinue
$removed = 0
foreach ($f in $stale) {
    if ($f.FullName -eq $old) { continue }
    try { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop; ++$removed } catch { }
}
if ($removed -gt 0) { Say ("  清掉了 {0} 个更早的备份" -f $removed) DarkGray }

if ($running) {
    Say '播放器仍在运行 —— 它执行的是内存里的旧代码，新版下次启动生效。' Yellow
} else {
    Say '播放器没在运行，下次启动就是新版。' Green
}
