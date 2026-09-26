<#
.SYNOPSIS
    抓取 Lyricus 各个界面的截图 —— 视觉验证用，可反复跑。

.DESCRIPTION
    Lyricus 有五个显示界面，全都在真机上、离屏单测覆盖不到：

        浮动面板（control_window）  独立置顶窗口，副屏用
        DUI 元素（dui_element）     嵌进 foobar2000 默认界面
        CUI 面板（cui_panel）       嵌进 Columns UI
        首选项页（prefs_page）      Preferences → Display → Lyricus
        色环取色器（color_picker）  点色块弹出来的那个

    这个脚本把"找窗口 + 截图"这两步固定下来。它们看着简单，但踩过的坑不少：

    ★ ① 别用 CopyFromScreen。
       它抓的是"屏幕上此刻显示的东西"，窗口被别的窗口盖住就会抓错 ——
       实测第一次抓 DUI 元素，抓回来的是当时的编辑器窗口。
       用 PrintWindow / GetWindowDC 直接跟窗口要像素，不受遮挡影响。

    ★ ② 必须声明 DPI 感知。
       不调 SetProcessDpiAwarenessContext 的话，GetWindowRect 返回的是被
       DWM 虚拟化过的坐标，200% 缩放下截图会整体错位（还会小一圈）。

    ★ ③ 多显示器下坐标可能是负的。
       本机虚拟屏幕是 5806x2039 @ (-2926,0)：左屏从 -2926 开始。
       任何"假设坐标从 0 开始"的写法都会在副屏上失效。

    ★ ④ 自绘窗口对 PrintWindow 的反应不一样。
       我们三个窗口都是 GDI 双缓冲自绘，理论上该响应 WM_PRINT，但谁也没实现它。
       实测 PrintWindow 能抓到内容，GetWindowDC 也能 —— 但**哪个管用会变**。
       所以两个都试，各自数一下颜色数，挑非空白的那个（见下面 Pick-BestCapture）。

.PARAMETER Target
    要抓哪些：all（默认）/ panel / dui / cui / main / prefs / wheel
    可以多选，逗号分隔。

.PARAMETER OutDir
    输出目录。默认 docs/visual/<yyyyMMdd-HHmmss>。
    传了就原样用（方便"同一批截图放一起做前后对比"）。

.PARAMETER List
    只列出找到的窗口和它们的矩形，不截图。排查"窗口到底在不在"时用。

.PARAMETER Restart
    先重启 foobar2000。改了 C++ 代码、守候进程装好之后要重启才生效，
    这一步和 watch-install.ps1 是同一个流程。

.PARAMETER Open
    抓完用默认看图程序打开目录。

.PARAMETER Settle
    一张图最多重抓几次，默认 3。
    「窗口存在」不等于「窗口有内容」：刚重启时进程和窗口都好了，
    但歌词还没加载，抓回来是一张近乎空白的图（实测 DUI 元素只有 5 种颜色）。
    颜色数 <= 2 时会等 1.5 秒重抓，最多试这么多次。

.EXAMPLE
    .\tools\capture-ui.ps1 -List
    看看现在有哪些窗口、各自多大、在哪块屏上。

.EXAMPLE
    .\tools\capture-ui.ps1 -Target all -Restart
    重启加载最新构建，然后五个界面各抓一张。

.EXAMPLE
    .\tools\capture-ui.ps1 -Target dui,panel -OutDir docs\visual\before
    只抓两个，放进指定目录（配合另一次 -OutDir after 做前后对比）。

.NOTES
    首选项页和色环取色器是**对话框**，得先手动打开才会出现在窗口列表里
    （View → Lyricus 外观设置...，然后点一个色块）。
    脚本抓不到"还没打开的窗口" —— 这是设计如此，不是缺陷：
    自动去点 UI 会引入一堆和被测对象无关的脆弱依赖。
#>

[CmdletBinding()]
param(
    [string[]]$Target = @('all'),
    [string]$OutDir,
    [switch]$List,
    [switch]$Restart,
    [switch]$Open,
    # 一张图最多重抓几次（窗口还没画好时等一会儿再试）。见 Save-WindowCaptureSettled。
    [int]$Settle = 3
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# Win32 互操作
# ---------------------------------------------------------------------------

# 重复调用 Add-Type 会报"类型已存在"，所以整块包一层 try。
try {
    Add-Type -Namespace LyricusCapture -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
// ★ PowerShell 把 $null 传给 C# 的 string 参数会变成**空字符串**，而 FindWindowEx
//   拿着空类名什么都找不到 —— 于是递归枚举在第一层就返回空，"DUI 元素不见了"。
//   包一层：这里的 null 才是真的 NULL（"任意类名/标题"）。
public static IntPtr FindFirstChild(IntPtr parent, IntPtr after) {
    return FindWindowEx(parent, after, null, null);
}
[DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr h);
[DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder buf, int max);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder buf, int max);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
[DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, int rop);
// EnumWindows：真正的顶层窗口枚举（含 owned 窗口）—— 见 Get-TopLevelWindows 的说明
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
public delegate bool EnumProc(IntPtr h, IntPtr p);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
'@ -ErrorAction Stop
} catch { }

# ★ ② DPI 感知必须在**任何**坐标查询之前设好。
#    -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
[void][LyricusCapture.Native]::SetProcessDpiAwarenessContext([IntPtr](-4))

Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# 窗口清单
# ---------------------------------------------------------------------------

# 每个界面一条。cls 是窗口类名，全部来自源码里的字面量：
#   control_window.cpp:35   kWindowClass
#   dui_element.cpp:77      DECLARE_WND_CLASS_EX
#   cui_panel.cpp:129       DECLARE_WND_CLASS_EX
#
# 两个对话框都是标准的 #32770，而 foobar2000 自己也用这个类（实测同时开着
# 6 个无关的 #32770），所以必须再按**标题**筛一道 —— 只看类名会抓一堆垃圾回来。
# title 是正则；首选项那个标题由宿主决定，中英文都收。
$Script:Targets = [ordered]@{
    'panel' = @{ cls = 'LyricusControlPanel';                      title = '';                  desc = '独立浮动面板';      note = '副屏置顶窗口' }
    'dui'   = @{ cls = '{1A7C3E90-2B41-4C58-9D6E-0F1A2B3C4D10}';   title = '';                  desc = 'DUI 元素';          note = '嵌在 foobar2000 默认界面里' }
    'cui'   = @{ cls = '{1A7C3E90-2B41-4C58-9D6E-0F1A2B3C4D11}';   title = '';                  desc = 'CUI 面板';          note = '嵌在 Columns UI 里' }
    # ★ 主窗口**按标题**判定，不按类名 —— DUI 和 CUI 的主窗口类名完全不同：
    #     DUI: {97E27FAA-C0B3-4b8e-A693-ED7881E99FC1}
    #     CUI: {E7076D1C-A7BF-4f39-B771-BCBE88F2A2A8}
    #   写死任一个，用户一换 UI 这个目标就失效（实测切到 CUI 后 -Restart
    #   直接报"找不到 foobar2000 主窗口"）。标题两边都含 "foobar2000"。
    'main'  = @{ cls = '';                                         title = 'foobar2000';        desc = 'foobar2000 主窗口';  note = '按标题匹配（主窗口类名随 UI 变）' }
    'prefs' = @{ cls = '#32770';                                   title = '首选项|Preferences'; desc = '首选项窗口';        note = 'View → Lyricus 外观设置...' }
    'wheel' = @{ cls = '#32770';                                   title = '选择颜色';           desc = '色环取色器';         note = '在首选项页里点一个色块' }
}

function Get-ClassOf([IntPtr]$Hwnd) {
    $sb = New-Object System.Text.StringBuilder 256
    [void][LyricusCapture.Native]::GetClassName($Hwnd, $sb, $sb.Capacity)
    return $sb.ToString()
}

function Get-TitleOf([IntPtr]$Hwnd) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][LyricusCapture.Native]::GetWindowTextW($Hwnd, $sb, $sb.Capacity)
    return $sb.ToString()
}

# 拿一个进程的**所有顶层窗口**。
#
# ★ 不能只用 Process.MainWindowHandle —— 它只给一个句柄，而且会挑中
#   LyricusControlPanel（那是个独立置顶窗口）。于是从它往下找，永远找不到
#   挂在 foobar2000 主窗口下面的 DUI / CUI 元素。实测就是这么漏掉的：
#   第一次跑只报出"1 个窗口"，而实际上有 3 个。
#   UIAutomation 的 RootElement 能看到所有顶层窗口，按 ProcessId 过滤即可。
# ★ 用 EnumWindows，**不用** UIAutomation 的 RootElement.Children。
#
# 【为什么换】UIAutomation 的 RootElement.Children **不含 owned 窗口**，
# 而 foobar2000 的首选项（类名 #32770）正是挂在主窗口下面的 owned window ——
# 于是 `-Target prefs` 一直抓到 0 张，尽管它的类名和标题都对得上。
# EnumWindows 给的是**真正的顶层窗口**（含 owned），按 PID 过滤即可。
#
# ⚠️ 顺带：UIAutomation 的 `TreeScope::Descendants` 在 foobar2000 上会**卡死**
#   （UI 树太大，遍历走不完）。所以这个文件里能不碰 UIA 就不碰。
#
# 历史（保留，说明为什么一开始用 UIA）：不能只用 Process.MainWindowHandle ——
# 它只给一个句柄，而且会挑中 LyricusControlPanel（独立置顶窗口），
# 从它往下找永远找不到挂在主窗口下面的 DUI / CUI 元素。
function Get-TopLevelWindows([int]$ProcessId) {
    $out = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [LyricusCapture.Native+EnumProc] {
        param($h, $p)
        $wpid = 0
        [void][LyricusCapture.Native]::GetWindowThreadProcessId($h, [ref]$wpid)
        if ($wpid -eq $ProcessId) { $out.Add($h) }
        return $true
    }
    [void][LyricusCapture.Native]::EnumWindows($cb, [IntPtr]::Zero)
    return $out
}

# 递归拿一个窗口的所有后代。DUI / CUI 元素嵌得很深（主窗口 → splitter → … → 元素），
# 只枚举一层是找不到的。
function Get-Descendants([IntPtr]$Parent) {
    $found = New-Object System.Collections.Generic.List[IntPtr]
    $child = [LyricusCapture.Native]::FindFirstChild($Parent, [IntPtr]::Zero)
    while ($child -ne [IntPtr]::Zero) {
        $found.Add($child)
        foreach ($d in (Get-Descendants $child)) { $found.Add($d) }
        $child = [LyricusCapture.Native]::FindFirstChild($Parent, $child)
    }
    return $found
}

function Get-FoobarWindows {
    $procs = @(Get-Process foobar2000 -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { throw "foobar2000 没在运行。先把它启动起来。" }

    # 按类名归类。同一个类可能出现多个（比如同时开了几个 CUI 面板），全都收着。
    $byClass = @{}
    foreach ($p in $procs) {
        $tops = @(Get-TopLevelWindows $p.Id)
        # 兜底：UIAutomation 偶尔拿不到（权限/时机），MainWindowHandle 至少给一个
        if ($tops.Count -eq 0 -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
            $tops = @($p.MainWindowHandle)
        }
        foreach ($h in $tops) {
            $all = @($h) + @(Get-Descendants $h)
            foreach ($w in $all) {
                $cls = Get-ClassOf $w
                if (-not $byClass.ContainsKey($cls)) {
                    $byClass[$cls] = New-Object System.Collections.Generic.List[IntPtr]
                }
                $byClass[$cls].Add($w)
            }
        }
    }

    $result = New-Object System.Collections.Generic.List[object]
    foreach ($key in $Script:Targets.Keys) {
        $spec = $Script:Targets[$key]

        # cls 为空 = 这个目标**按标题**找（只有 main 是这样，理由见上面的注释）。
        # 那种情况下要遍历所有顶层窗口，而不是查 byClass 表。
        $cands = if ($spec.cls -eq '') { $byClass.Values | ForEach-Object { $_ } }
                 elseif ($byClass.ContainsKey($spec.cls)) { $byClass[$spec.cls] }
                 else { @() }

        foreach ($h in $cands) {
            if (-not [LyricusCapture.Native]::IsWindowVisible($h)) { continue }
            # 标题再筛一道：只有 #32770 需要，其余目标的 title 是空串（不过滤）
            if ($spec.title -ne '' -and (Get-TitleOf $h) -notmatch $spec.title) { continue }
            $r = New-Object LyricusCapture.Native+RECT
            [void][LyricusCapture.Native]::GetWindowRect($h, [ref]$r)
            $w = $r.R - $r.L; $ht = $r.B - $r.T
            if ($w -le 0 -or $ht -le 0) { continue }
            $result.Add([pscustomobject]@{
                Kind   = $key
                Desc   = $spec.desc
                Hwnd   = $h
                Left   = $r.L
                Top    = $r.T
                Width  = $w
                Height = $ht
            })
        }
    }
    return $result
}

# ---------------------------------------------------------------------------
# 截图
# ---------------------------------------------------------------------------

# 数一下图里有多少种颜色（隔 4 像素采样，够判断"是不是空白"了）。
function Get-ColorCount([System.Drawing.Bitmap]$Bmp) {
    $set = New-Object 'System.Collections.Generic.HashSet[int]'
    $stepX = [Math]::Max(1, [int]($Bmp.Width  / 64))
    $stepY = [Math]::Max(1, [int]($Bmp.Height / 64))
    for ($y = 0; $y -lt $Bmp.Height; $y += $stepY) {
        for ($x = 0; $x -lt $Bmp.Width; $x += $stepX) {
            [void]$set.Add($Bmp.GetPixel($x, $y).ToArgb())
        }
    }
    return $set.Count
}

# ★ ④ 两种抓法都试，挑内容更丰富的那个。
#
# 为什么不能只留一种：PrintWindow 会往窗口发 WM_PRINT / WM_PRINTCLIENT，
# 而我们的三个自绘窗口**都没实现**这两个消息 —— 它之所以还能出图，
# 靠的是 PW_RENDERFULLCONTENT 让系统走另一条路，这条路的实际行为
# 在不同 Windows 版本上不一样。GetWindowDC 则是直接把窗口表面拷出来。
# 两条路都可能在某次改动后失效，与其猜哪个对，不如都跑一遍看结果。
function Save-WindowCapture {
    param([IntPtr]$Hwnd, [int]$W, [int]$H, [string]$Path)

    $best = $null
    $bestColors = -1
    $bestHow = ''

    foreach ($how in @('windowdc', 'printwindow')) {
        $bmp = New-Object System.Drawing.Bitmap($W, $H)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        try {
            if ($how -eq 'windowdc') {
                $src = [LyricusCapture.Native]::GetWindowDC($Hwnd)
                try {
                    [void][LyricusCapture.Native]::BitBlt($hdc, 0, 0, $W, $H, $src, 0, 0, 0x00CC0020)  # SRCCOPY
                } finally {
                    [void][LyricusCapture.Native]::ReleaseDC($Hwnd, $src)
                }
            } else {
                # 2 = PW_RENDERFULLCONTENT
                [void][LyricusCapture.Native]::PrintWindow($Hwnd, $hdc, 2)
            }
        } catch {
            # 单种方法失败不影响另一种
        } finally {
            $g.ReleaseHdc($hdc)
            $g.Dispose()
        }

        $colors = Get-ColorCount $bmp
        if ($colors -gt $bestColors) {
            if ($null -ne $best) { $best.Dispose() }
            $best = $bmp
            $bestColors = $colors
            $bestHow = $how
        } else {
            $bmp.Dispose()
        }
    }

    if ($null -eq $best) { return $null }
    $best.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $best.Dispose()

    return [pscustomobject]@{ Colors = $bestColors; How = $bestHow }
}

# 抓一张"画好了"的图。
#
# ★ 为什么要重试：窗口存在 ≠ 窗口有内容。刚重启时进程起来了、窗口也建好了，
#   但歌词还没加载完，这时抓回来是一张近乎空白的图 —— 实测 DUI 元素只有
#   5 种颜色（画好了是 100 上下）。纯色图对视觉验证毫无价值，而且很容易
#   被误读成"界面坏了"。所以颜色数太低就等一会儿再抓，最多试 -Settle 次。
function Save-WindowCaptureSettled {
    param(
        [IntPtr]$Hwnd, [int]$W, [int]$H, [string]$Path,
        [int]$Settle = 3, [int]$WaitMs = 1500
    )

    $last = $null
    for ($i = 0; $i -lt [Math]::Max(1, $Settle); $i++) {
        $info = Save-WindowCapture -Hwnd $Hwnd -W $W -H $H -Path $Path
        if ($null -eq $info) { return $null }
        $last = $info
        if ($info.Colors -gt 2) { return $info }     # 有内容了
        if ($i -lt $Settle - 1) { Start-Sleep -Milliseconds $WaitMs }
    }
    return $last
}

# ---------------------------------------------------------------------------
# 重启
# ---------------------------------------------------------------------------

# 和 watch-install.ps1 同一套流程：给主窗口发 WM_CLOSE（那会真的退出，
# 不是最小化），然后等新进程起来。
function Restart-Foobar {
    $procs = @(Get-Process foobar2000 -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { throw "foobar2000 没在运行，没什么可重启的。" }

    $oldPid = $procs[0].Id
    $main = [IntPtr]::Zero
    foreach ($p in $procs) {
        # 用和截图同一条枚举路径，而且**按标题**判定主窗口（理由见
        # $Script:Targets 里 main 那条注释 —— DUI / CUI 的主窗口类名不同）。
        foreach ($h in (Get-TopLevelWindows $p.Id)) {
            if ((Get-TitleOf $h) -match $Script:Targets['main'].title) { $main = $h; break }
        }
        if ($main -ne [IntPtr]::Zero) { break }
    }
    if ($main -eq [IntPtr]::Zero) { throw "找不到 foobar2000 主窗口，没法优雅退出。" }

    Write-Host "  给主窗口发 WM_CLOSE..." -ForegroundColor DarkGray
    [void][LyricusCapture.Native]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)

    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 1
        $np = @(Get-Process foobar2000 -ErrorAction SilentlyContinue)
        if ($np.Count -gt 0 -and $np[0].Id -ne $oldPid) {
            Write-Host "  新进程 PID $($np[0].Id)" -ForegroundColor DarkGray
            # 窗口建起来还要一会儿：进程在、窗口还没出现是正常的
            Start-Sleep -Seconds 4
            return $np[0]
        }
    }

    # ★ 没人拉起来 —— 自己启动。
    #
    # 【为什么不能指望守候进程】watch-install.ps1 只在**磁盘上的 DLL 变了**
    # 的时候才重新加载并重启播放器。而"改 C++ -> 编译 -> 重启看效果"这个循环里
    # 常见的情形恰恰是：DLL 上一轮就已经是最新的，守候进程认为无事可做，
    # 于是关掉播放器之后就没人负责拉起来了 —— 实测就这样把播放器关在了一边。
    Write-Host "  没有自动拉起，自己启动..." -ForegroundColor DarkYellow
    $exe = Find-FoobarExe
    if (-not $exe) { throw "找不到 foobar2000.exe，请手动启动。" }

    # ⚠️ 不要给 Start-Process 传文件参数：
    #    `foobar2000.exe <file>` 会**替换当前播放列表**（这个坑踩过一次，
    #    清掉了一个 105 首的列表）。直接启动，什么参数都不带。
    Start-Process $exe
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 1
        $np = @(Get-Process foobar2000 -ErrorAction SilentlyContinue)
        if ($np.Count -gt 0) {
            Write-Host "  新进程 PID $($np[0].Id)" -ForegroundColor DarkGray
            Start-Sleep -Seconds 4
            return $np[0]
        }
    }
    throw "启动了 foobar2000.exe，但进程一直没出现。"
}

# 找 foobar2000.exe。先看常规安装位置，再问注册表。
function Find-FoobarExe {
    $cands = @(
        "$env:ProgramFiles\foobar2000\foobar2000.exe",
        "${env:ProgramFiles(x86)}\foobar2000\foobar2000.exe",
        "$env:LOCALAPPDATA\Programs\foobar2000\foobar2000.exe"
    )
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }

    $dir = (Get-ItemProperty 'HKCU:\Software\foobar2000' -ErrorAction SilentlyContinue).InstallDir
    if ($dir) {
        $p = Join-Path $dir 'foobar2000.exe'
        if (Test-Path $p) { return $p }
    }
    return $null
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

Write-Host "`n=== Lyricus 视觉截图 ===" -ForegroundColor Cyan

if ($Restart) {
    Write-Host "`n[重启] 加载最新构建" -ForegroundColor Yellow
    $null = Restart-Foobar
    # 说清楚，免得把"没播放"误读成"界面坏了"：
    # 重启后 foobar2000 是停止状态，没有曲目就没有歌词，歌词区域本来就是空的。
    Write-Host "  （重启后是停止状态，歌词区会是空的 —— 属正常，不是界面坏了）" -ForegroundColor DarkGray
}

$wins = @(Get-FoobarWindows)

if ($wins.Count -eq 0) {
    Write-Host "`n一个目标窗口都没找到。可能原因：" -ForegroundColor Red
    Write-Host "  * foobar2000 刚启动，布局还没建完 —— 等几秒再跑"
    Write-Host "  * DUI / CUI 里根本没加 Lyricus 元素"
    exit 1
}

# ---- -List：只报告 ----
if ($List) {
    Write-Host "`n找到 $($wins.Count) 个窗口：`n"
    $wins | Format-Table -AutoSize @{n='类型';e={$_.Kind}},
                                   @{n='说明';e={$_.Desc}},
                                   @{n='尺寸';e={"$($_.Width)x$($_.Height)"}},
                                   @{n='位置';e={"$($_.Left),$($_.Top)"}},
                                   @{n='hwnd';e={$_.Hwnd}}
    exit 0
}

# ---- 选目标 ----
$want = @()
if ($Target -contains 'all') {
    $want = @($Script:Targets.Keys)
} else {
    foreach ($t in $Target) {
        if (-not $Script:Targets.Contains($t)) {
            Write-Host "未知目标 '$t'。可选：$($Script:Targets.Keys -join ', '), all" -ForegroundColor Red
            exit 1
        }
        $want += $t
    }
}

if (-not $OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) "docs\visual\$stamp"
}
if (-not (Test-Path $OutDir)) { [void](New-Item -ItemType Directory -Path $OutDir -Force) }
$OutDir = (Resolve-Path $OutDir).Path

Write-Host "`n输出目录: $OutDir`n" -ForegroundColor DarkGray

$captured = 0
foreach ($w in ($wins | Where-Object { $want -contains $_.Kind })) {
    $name = if ($wins.Where({ $_.Kind -eq $w.Kind }).Count -gt 1) {
        "$($w.Kind)-$($w.Hwnd)"      # 同类有多个时带上 hwnd，免得互相覆盖
    } else { $w.Kind }

    $path = Join-Path $OutDir "$name.png"
    $info = Save-WindowCaptureSettled -Hwnd $w.Hwnd -W $w.Width -H $w.Height -Path $path -Settle $Settle

    if ($null -eq $info) {
        Write-Host ("  [失败] {0,-8} {1}" -f $w.Kind, $w.Desc) -ForegroundColor Red
        continue
    }

    # 颜色数太少说明抓到了一张纯色图（窗口还没画、或者两种抓法都没拿到内容）
    $flag = if ($info.Colors -le 2) { '  ⚠ 疑似空白' } else { '' }
    $color = if ($info.Colors -le 2) { 'Yellow' } else { 'Green' }
    Write-Host ("  [{0,-8}] {1,-16} {2,5}x{3,-5} {4} 色/{5}{6}" -f
        $w.Kind, $w.Desc, $w.Width, $w.Height, $info.Colors, $info.How, $flag) -ForegroundColor $color
    $captured++
}

Write-Host "`n抓到 $captured 张。`n" -ForegroundColor Cyan

# 提醒哪些目标没找到 —— 多半是"还没打开"，不是脚本坏了
foreach ($t in $want) {
    if (-not ($wins | Where-Object { $_.Kind -eq $t })) {
        Write-Host "  (没有 '$t'：$($Script:Targets[$t].note))" -ForegroundColor DarkYellow
    }
}

if ($Open) { Invoke-Item $OutDir }
