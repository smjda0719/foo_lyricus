<#
  截取指定窗口的屏幕画面。

  用法：
    .\capture-window.ps1                          # 截 Lyricus 面板
    .\capture-window.ps1 -ClassFilter "*Lyricus*"
    .\capture-window.ps1 -ProcessName foobar2000
    .\capture-window.ps1 -FullScreen              # 截整个虚拟桌面（多屏）

  两个踩过的坑，都记在这儿：

  1) DPI 感知必须在最开头设置。
     pwsh 默认是 DPI 不感知进程，此时 GetWindowRect 返回的是「虚拟化」坐标
     （按 96 DPI 缩放后的逻辑值），而抓屏得到的是物理像素 —— 两者对不上，
     截出来会偏移、缺边、尺寸不符。
     例如 200% 缩放下，460x150 的窗口会报成 230x75。

  2) 这里用 EnumWindows 匹配类名，不用 FindWindow。
     实测同一时刻 FindWindowW("LyricusControlPanel", NULL) 返回 0，
     而 EnumWindows 能正常枚举出该类名的窗口。原因未查明，绕过即可。

  抓屏方式用 CopyFromScreen 而非 PrintWindow：
    本项目窗口背景由 DWM 合成（Mica/Acrylic），PrintWindow 走窗口自身绘制路径，
    拿不到合成后的图层，通常得到全黑。代价是要求目标窗口可见、未被遮挡。
#>

[CmdletBinding()]
param(
    [string]$ClassName = 'LyricusControlPanel',
    [string]$ProcessName = 'foobar2000',
    [int]$Margin = 20,
    [string]$OutFile,
    [switch]$FullScreen
)

$ErrorActionPreference = 'Stop'

if (-not ('WinCap.Native' -as [type])) {
    Add-Type -Namespace WinCap -Name Native -MemberDefinition @'
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr p);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr h);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L, T, R, B; }
'@
}

# DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
$dpiOk = [WinCap.Native]::SetProcessDpiAwarenessContext([IntPtr](-4))
Write-Host ("DPI 感知: {0}" -f $(if ($dpiOk) { 'Per-Monitor V2 ✓' } else { '设置失败（可能已被清单固定）' }))

Add-Type -AssemblyName System.Drawing

function Get-WindowInfo([IntPtr]$h) {
    $cls = New-Object System.Text.StringBuilder 256
    [void][WinCap.Native]::GetClassNameW($h, $cls, 256)
    $txt = New-Object System.Text.StringBuilder 512
    [void][WinCap.Native]::GetWindowTextW($h, $txt, 512)
    $pid_ = 0
    [void][WinCap.Native]::GetWindowThreadProcessId($h, [ref]$pid_)
    $r = New-Object WinCap.Native+RECT
    [void][WinCap.Native]::GetWindowRect($h, [ref]$r)
    [pscustomobject]@{
        Hwnd = $h; Class = $cls.ToString(); Title = $txt.ToString(); Pid = $pid_
        Visible = [WinCap.Native]::IsWindowVisible($h)
        Rect = $r
    }
}

function Find-TargetWindow {
    $script:target = [IntPtr]::Zero
    $script:all = New-Object System.Collections.ArrayList
    $cb = [WinCap.Native+EnumProc]{
        param($h, $p)
        $info = Get-WindowInfo $h
        [void]$script:all.Add($info)
        if ($script:target -eq [IntPtr]::Zero -and
            $info.Visible -and
            $info.Class -eq $ClassName -and
            ($info.Pid -eq $targetPid)) {
            $script:target = $h
        }
        return $true
    }
    [void][WinCap.Native]::EnumWindows($cb, [IntPtr]::Zero)
}

$targetPid = (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1).Id
if (-not $targetPid) { Write-Host "进程 '$ProcessName' 未运行" -ForegroundColor Yellow; exit 3 }
Write-Host ("进程 {0}  PID={1}" -f $ProcessName, $targetPid)

Find-TargetWindow

if ($FullScreen) {
    $x = [WinCap.Native]::GetSystemMetrics(76); $y = [WinCap.Native]::GetSystemMetrics(77)
    $w = [WinCap.Native]::GetSystemMetrics(78); $h = [WinCap.Native]::GetSystemMetrics(79)
    Write-Host ("虚拟桌面: {0},{1} {2}x{3}" -f $x, $y, $w, $h)
} elseif ($script:target -ne [IntPtr]::Zero) {
    $t = Get-WindowInfo $script:target
    $r = $t.Rect
    $dpi = [WinCap.Native]::GetDpiForWindow($script:target)
    Write-Host ("命中窗口 0x{0:X}  class='{1}'  title='{2}'" -f $script:target.ToInt64(), $t.Class, $t.Title) -ForegroundColor Green
    Write-Host ("  窗口 DPI = {0}  (缩放 {1}%)" -f $dpi, [math]::Round($dpi/96*100))
    Write-Host ("  物理矩形 = {0},{1} .. {2},{3}   尺寸 {4}x{5}" -f $r.L, $r.T, $r.R, $r.B, ($r.R-$r.L), ($r.B-$r.T))
    $x = $r.L - $Margin; $y = $r.T - $Margin
    $w = ($r.R - $r.L) + $Margin*2; $h = ($r.B - $r.T) + $Margin*2
} else {
    Write-Host ("没找到 class='{0}' 的可见窗口。该进程的顶层窗口：" -f $ClassName) -ForegroundColor Yellow
    $script:all | Where-Object { $_.Pid -eq $targetPid } |
        ForEach-Object { Write-Host ("  [vis={0,-5}] {1,-40} {2}" -f $_.Visible, $_.Class, $_.Title) }
    exit 2
}

$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
try {
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size($w, $h)),
                      [System.Drawing.CopyPixelOperation]::SourceCopy)
} finally { $g.Dispose() }

if (-not $OutFile) {
    $dir = Join-Path $PSScriptRoot '..\captures'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $OutFile = Join-Path $dir ("lyricus-{0}.png" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null
}

$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host ("`n已保存: {0}  ({1:N0} B)" -f (Resolve-Path $OutFile).Path, (Get-Item $OutFile).Length) -ForegroundColor Green
Write-Output (Resolve-Path $OutFile).Path
