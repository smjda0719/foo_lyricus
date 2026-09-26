<#
.SYNOPSIS
    给 foobar2000 的**首选项页**截图（不用重启播放器）。

.DESCRIPTION
    【为什么单独一个脚本】`capture-ui.ps1` 那套走 UIAutomation，在 foobar2000
    上会卡死（它的 UI 树太大，TreeScope::Descendants 遍历走不完），而且它的
    `prefs` 目标一直找不到首选项窗口。这里全程改用 Win32 直调，快且稳。

    【链路】每一步都换掉了"显然的做法"，理由都在这儿：

      打开   Ctrl+P 发给**主窗口**
             ⚠️ 主窗口**没有标准菜单**（`GetMenu` 返回 0，是自绘菜单条），
                所以"枚举菜单找 Preferences 再发 WM_COMMAND"那条路根本不通。

      找窗口 EnumWindows + 标题匹配
             ⚠️ 不用 `Process.MainWindowHandle` —— 它会挑中浮动面板
                （那是个独立置顶窗口），这条已经踩过两次。
             ⚠️ 不用 UIAutomation 的 Descendants —— 会卡死。

      发按键 keybd_event
             ⚠️ 不用 `SendInput` —— 它的 INPUT 结构含 union，自己拼容易把
                cbSize 算错（实测 28，而 Win32 要 40），而它会**静默返回 0**：
                什么都不发，也不报错。

      聚焦   SetForegroundWindow(主窗口)
             ⚠️ 快捷键是发给**前台窗口**的，不是发给"我们指定的那个 hwnd"。

      截图   PrintWindow(hwnd, dc, 2)   （2 = PW_RENDERFULLCONTENT）
             ⚠️ 不用 `CopyFromScreen` —— 它抓的是"屏幕上此刻显示的东西"，
                窗口被遮挡就抓错了。
             ⚠️ **前提：被截的窗口必须处理 `WM_PRINTCLIENT`。**
                全自绘窗口不处理它时，PrintWindow 只走到默认的背景处理 ——
                **截图里一片空白，而窗口在屏幕上完全正常**。
                这个坑害我误判过一次（发了张"滚到底变空白"的图，
                用户说"是你的截图脚本问题，我实际看没有问题"）。
                `prefs_page.cpp` 里有现成例子：把绘制抽成 `PaintTo(dc, rc)`，
                `WM_PAINT` 与 `WM_PRINTCLIENT` 共用一份。

      滚动   SendMessage(WM_VSCROLL, SB_TOP / SB_BOTTOM)
             ⚠️ 用系统消息，不用模拟鼠标滚轮 —— 后者依赖窗口有焦点、
                鼠标位置对，而且会真的动用户的鼠标。

    【脚本还会算"有内容像素占比"】那是判断"这张截图到底画出来没有"的
    客观指标。全白 = 0%，正常页面约 30~35%。当初就是靠它在 0.0% -> 33.9%
    的对比里确认 WM_PRINTCLIENT 修好了。看图判断反而容易看走眼。

.PARAMETER OutDir
    输出目录，默认 %TEMP%\lyricus-prefs。

.PARAMETER Open
    首选项没开着时，先发 Ctrl+P 打开它。

.PARAMETER Scroll
    额外截「滚到底」和「回顶部」两张 —— 用来验证滚动是否工作。

.PARAMETER List
    只列出找到的窗口，不截图。

.EXAMPLE
    .\tools\prefs-shot.ps1
    首选项已经开着时，直接截一张。

.EXAMPLE
    .\tools\prefs-shot.ps1 -Open -Scroll
    打开首选项，然后截三张（初始 / 滚到底 / 回顶部）。

.NOTES
    前提：foobar2000 正在运行，且键盘焦点可以被抢（脚本会 SetForegroundWindow）。
#>

[CmdletBinding()]
param(
    [string]$OutDir,
    [switch]$Open,
    [switch]$Scroll,
    [switch]$List
)

$ErrorActionPreference = 'Stop'

if (-not $OutDir) { $OutDir = Join-Path $env:TEMP 'lyricus-prefs' }
if (-not (Test-Path $OutDir)) { [void](New-Item -ItemType Directory -Path $OutDir -Force) }

# ---------------------------------------------------------------------------
# Win32
# ---------------------------------------------------------------------------
Add-Type -Namespace LyricusShot -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int m);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int m);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
[DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
public delegate bool EnumProc(IntPtr h, IntPtr p);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
'@

# DPI 感知必须在**任何窗口查询之前**声明。
# 不声明的话 GetWindowRect 返回的是 DWM 虚拟化过的坐标，200% 缩放下截图整体错位。
[void][LyricusShot.Native]::SetProcessDpiAwarenessContext([IntPtr](-4))
Add-Type -AssemblyName System.Drawing

$VK_CONTROL = 0x11
$VK_P     = 0x50
$KEYUP    = 0x0002
$WM_VSCROLL = 0x0115
$SB_TOP     = 6
$SB_BOTTOM  = 7

$foobar = Get-Process foobar2000 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $foobar) { throw 'foobar2000 没在运行。' }
$foobarPid = $foobar.Id

# ---------------------------------------------------------------------------
# 窗口查找：EnumWindows + 标题正则
# ---------------------------------------------------------------------------
function Get-FoobarWindows {
    $script:found = New-Object System.Collections.Generic.List[object]
    $cb = [LyricusShot.Native+EnumProc] {
        param($h, $p)
        $wpid = 0
        [void][LyricusShot.Native]::GetWindowThreadProcessId($h, [ref]$wpid)
        if ($wpid -ne $foobarPid) { return $true }
        if (-not [LyricusShot.Native]::IsWindowVisible($h)) { return $true }
        $t = New-Object System.Text.StringBuilder 512
        [void][LyricusShot.Native]::GetWindowTextW($h, $t, 512)
        $c = New-Object System.Text.StringBuilder 256
        [void][LyricusShot.Native]::GetClassNameW($h, $c, 256)
        $r = New-Object LyricusShot.Native+RECT
        [void][LyricusShot.Native]::GetWindowRect($h, [ref]$r)
        $script:found.Add([pscustomobject]@{
            Hwnd = $h; Title = $t.ToString(); Class = $c.ToString()
            W = $r.R - $r.L; H = $r.B - $r.T; Left = $r.L; Top = $r.T
        })
        return $true
    }
    [void][LyricusShot.Native]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Find-Window([string]$titlePattern, [string]$classPattern) {
    foreach ($w in (Get-FoobarWindows)) {
        if ($titlePattern -and $w.Title -notlike $titlePattern) { continue }
        if ($classPattern -and $w.Class -notlike $classPattern) { continue }
        return $w
    }
    return $null
}

# ---------------------------------------------------------------------------
# 打开首选项
# ---------------------------------------------------------------------------
function Open-Preferences {
    # ⚠️ 快捷键发给**前台窗口**，所以要先把主窗口弄到前台。
    $main = Find-Window -titlePattern '*foobar2000*'
    if (-not $main) { throw '找不到 foobar2000 主窗口（标题里应含 foobar2000）。' }

    [void][LyricusShot.Native]::SetForegroundWindow($main.Hwnd)
    Start-Sleep -Milliseconds 500

    [LyricusShot.Native]::keybd_event($VK_CONTROL, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 70
    [LyricusShot.Native]::keybd_event($VK_P, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 70
    [LyricusShot.Native]::keybd_event($VK_P, 0, $KEYUP, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 70
    [LyricusShot.Native]::keybd_event($VK_CONTROL, 0, $KEYUP, [IntPtr]::Zero)

    # 首选项窗口要几百毫秒才起来；轮询比睡死更稳
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 200
        $w = Find-Window -titlePattern 'Preferences*'
        if (-not $w) { $w = Find-Window -titlePattern '首选项*' }
        if ($w) { return $w }
    }
    return $null
}

# ---------------------------------------------------------------------------
# 截图 + 内容占比
# ---------------------------------------------------------------------------
function Save-WindowShot($win, [string]$path) {
    $bmp = New-Object System.Drawing.Bitmap($win.W, $win.H)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $dc  = $g.GetHdc()
    # 2 = PW_RENDERFULLCONTENT
    [void][LyricusShot.Native]::PrintWindow($win.Hwnd, $dc, 2)
    $g.ReleaseHdc($dc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)

    # 有内容像素占比：全白 = 0%（说明没画出来，多半是没接 WM_PRINTCLIENT），
    # 正常页面 30~35%。取中间区域，避开标题栏和边框。
    $x0 = [int]($win.W * 0.30); $x1 = [int]($win.W * 0.95)
    $y0 = [int]($win.H * 0.08); $y1 = [int]($win.H * 0.95)
    $dark = 0; $tot = 0
    for ($y = $y0; $y -lt $y1; $y += 4) {
        for ($x = $x0; $x -lt $x1; $x += 4) {
            $c = $bmp.GetPixel($x, $y); $tot++
            if ($c.R -lt 230 -or $c.G -lt 230 -or $c.B -lt 230) { $dark++ }
        }
    }
    $bmp.Dispose()
    $pct = if ($tot -gt 0) { [math]::Round(100.0 * $dark / $tot, 1) } else { 0 }
    return [pscustomobject]@{ Path = $path; Pct = $pct }
}

function Invoke-Scroll($win, [int]$sbCode) {
    [void][LyricusShot.Native]::SendMessageW($win.Hwnd, $WM_VSCROLL, [IntPtr]$sbCode, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== Lyricus 首选项截图 ===' -ForegroundColor Cyan

if ($List) {
    foreach ($w in (Get-FoobarWindows)) {
        Write-Host ("  [{0,-22}] {1,-46} {2}x{3}" -f $w.Class, $w.Title, $w.W, $w.H)
    }
    Write-Host ''
    exit 0
}

$prefs = Find-Window -titlePattern 'Preferences*'
if (-not $prefs) { $prefs = Find-Window -titlePattern '首选项*' }

if (-not $prefs) {
    if (-not $Open) {
        Write-Host '  首选项没开着。加 -Open 让它自己打开。' -ForegroundColor Yellow
        exit 1
    }
    Write-Host '  首选项没开着，发 Ctrl+P...'
    $prefs = Open-Preferences
}

if (-not $prefs) {
    Write-Host '  ✗ 打不开首选项窗口。' -ForegroundColor Red
    exit 1
}

$cr = New-Object LyricusShot.Native+RECT
[void][LyricusShot.Native]::GetClientRect($prefs.Hwnd, [ref]$cr)
Write-Host ("  窗口 $($prefs.W)x$($prefs.H)   客户区 $($cr.R-$cr.L)x$($cr.B-$cr.T)")

$shots = @()
$shots += Save-WindowShot $prefs (Join-Path $OutDir 'prefs.png')

if ($Scroll) {
    Invoke-Scroll $prefs $SB_BOTTOM
    $shots += Save-WindowShot $prefs (Join-Path $OutDir 'prefs-bottom.png')
    Invoke-Scroll $prefs $SB_TOP
    $shots += Save-WindowShot $prefs (Join-Path $OutDir 'prefs-top.png')
}

Write-Host ''
foreach ($s in $shots) {
    $flag = if ($s.Pct -lt 1.0) { '  <-- ⚠️ 几乎空白，检查窗口有没有接 WM_PRINTCLIENT' } else { '' }
    Write-Host ("  {0,-22} 有内容 {1,5}%{2}" -f (Split-Path $s.Path -Leaf), $s.Pct, $flag)
}
Write-Host ''
Write-Host "  输出目录: $OutDir" -ForegroundColor Green
Write-Host ''
