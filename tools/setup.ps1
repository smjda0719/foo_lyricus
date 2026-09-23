<#
  获取 Lyricus 的全部外部依赖。

  为什么需要它：
    `3rdparty/` 被 .gitignore 排除了（体积 277 MB，且 foobar2000 SDK 与 WTL
    各有自己的许可协议，不适合入版本库）。所以新 clone 下来的仓库**是编不了的**，
    必须先跑这个脚本重建依赖。

  三个依赖：
    1. foobar2000 SDK   —— 官方站点的 .7z（单个文件）
    2. WTL 10           —— SDK 不自带 atlapp.h，libPPUI 编译需要它
    3. lunasvg          —— 渲染 SVG 图标（纯 CPU 光栅化，MIT）

  网络策略（应对不稳定网络）：
    * GitHub 的 /archive/refs/heads/*.zip 是**服务端即时生成**的，不支持 HTTP Range，
      断点续传无效 —— 所以 WTL 和 lunasvg 都改成**按单个文件下载**。
      单文件小，失败重试代价低，而且 raw.githubusercontent.com 支持 Range。
    * 所有下载都走 tools/fetch.ps1（带续传 + 退避重试 + 大小校验）。

  用法：
    .\setup.ps1                       # 缺什么下什么
    .\setup.ps1 -Force                # 全部重新下载
    .\setup.ps1 -Proxy ''             # 不走代理
#>

[CmdletBinding()]
param(
    [string]$Proxy = 'http://127.0.0.1:8902',
    [switch]$Force,
    [switch]$SkipSdk,
    [switch]$SkipWtl,
    [switch]$SkipLunasvg,
    [switch]$SkipCui
)

$ErrorActionPreference = 'Continue'

$root     = Split-Path -Parent $PSScriptRoot
$third    = Join-Path $root '3rdparty'
$download = Join-Path $root 'downloads'
$fetch    = Join-Path $PSScriptRoot 'fetch.ps1'
$sz       = 'C:\Program Files\7-Zip\7z.exe'

# columns_ui-sdk 的锁定版本。要升级就改这里，然后重新编译验证。
# 许可 0BSD（比 MIT/ISC 还宽松，连署名都不要求）。
$CuiSdkCommit = '69972e36febc4bfc2685fa1a7620d0ee8789e10f'   # 2026-09-14

function Say($msg, $color = 'Gray') { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $msg) -ForegroundColor $color }
function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

New-Item -ItemType Directory -Force -Path $third, $download | Out-Null

if (-not (Test-Path $fetch)) { Say "找不到 tools/fetch.ps1" Red; exit 1 }
if (-not (Test-Path $sz))    { Say "找不到 7-Zip：$sz（SDK 是 .7z 格式，必需）" Red; exit 1 }

# 下载单个文件（存在且非空则跳过，除非 -Force）
function Get-One([string]$Url, [string]$Out, [long]$Expect = 0) {
    $leaf = Split-Path -Leaf $Out
    if (-not $Force -and (Test-Path $Out) -and (Get-Item $Out).Length -gt 0) {
        if ($Expect -le 0 -or (Get-Item $Out).Length -eq $Expect) {
            Say ("  跳过（已存在） {0}" -f $leaf) DarkGray
            return $true
        }
    }
    $null = & $fetch -Url $Url -Out $Out -Proxy $Proxy -ExpectSize $Expect -Quiet
    if ($LASTEXITCODE -ne 0) { Say ("  失败 {0}" -f $Url) Red; return $false }
    return $true
}

# 按 GitHub 仓库的源码文件清单逐个下载
function Get-GithubTree([string]$Owner, [string]$Repo, [string]$Branch,
                        [string]$PathPrefix, [string]$DestRoot, [string]$IncludeRegex) {
    $api = "https://api.github.com/repos/$Owner/$Repo/git/trees/$Branch`?recursive=1"
    $tmp = Join-Path $download "_tree_$Repo.json"
    $null = & $fetch -Url $api -Out $tmp -Proxy $Proxy -Quiet
    if ($LASTEXITCODE -ne 0) { Say "  拉取文件清单失败" Red; return $false }

    $tree = (Get-Content $tmp -Raw | ConvertFrom-Json).tree |
            Where-Object { $_.type -eq 'blob' -and $_.path -like "$PathPrefix*" -and $_.path -match $IncludeRegex }

    if (-not $tree) { Say "  文件清单为空（前缀 $PathPrefix）" Red; return $false }

    Say ("  共 {0} 个文件，{1:N0} KB" -f ($tree | Measure-Object).Count,
         (($tree | Measure-Object size -Sum).Sum / 1KB))

    $ok = 0; $fail = 0
    foreach ($f in $tree) {
        $rel  = $f.path.Substring($PathPrefix.Length)
        $out  = Join-Path $DestRoot ($rel -replace '/', '\')
        $url  = "https://raw.githubusercontent.com/$Owner/$Repo/$Branch/$($f.path)"
        if (Get-One $url $out $f.size) { $ok++ } else { $fail++ }
    }
    Say ("  完成：成功 {0}，失败 {1}" -f $ok, $fail) $(if ($fail -eq 0) { 'Green' } else { 'Yellow' })
    return ($fail -eq 0)
}

# ---------------------------------------------------------------------------
# 1. foobar2000 SDK
# ---------------------------------------------------------------------------
if (-not $SkipSdk) {
    Step '1/3  foobar2000 SDK'

    $sdkDir = Join-Path $third 'foobar2000'
    if ((Test-Path $sdkDir) -and -not $Force) {
        Say "  已存在，跳过（-Force 可强制重下）" DarkGray
    } else {
        $version = '2026-09-17'
        $url = "https://www.foobar2000.org/downloads/SDK-$version.7z"
        $pkg = Join-Path $download "SDK-$version.7z"

        # 官方站点是静态文件，理论上支持 Range；fetch.ps1 会自动处理不支持的情况
        if (Get-One $url $pkg) {
            Say "  解压到 3rdparty\"
            & $sz x $pkg "-o$third" -y | Out-Null
            if ($LASTEXITCODE -ne 0) { Say "  解压失败" Red } else { Say "  完成" Green }
        }
    }
}

# ---------------------------------------------------------------------------
# 2. WTL 10
#
# SDK 不自带 atlapp.h，libPPUI 与 foobar2000_sdk_helpers 都依赖它。
# 逐个头文件下载：仓库自带的 Releases/*.zip 曾经下到过截断包，
# 而逐个文件下载每个都很小、失败可续。
# ---------------------------------------------------------------------------
if (-not $SkipWtl) {
    Step '2/4  WTL 10'

    $wtlInc = Join-Path $third 'WTL\Include'
    if ((Test-Path (Join-Path $wtlInc 'atlapp.h')) -and -not $Force) {
        Say "  已存在，跳过" DarkGray
    } else {
        New-Item -ItemType Directory -Force -Path $wtlInc | Out-Null
        $ok = Get-GithubTree -Owner 'Win32-WTL' -Repo 'WTL' -Branch 'master' `
                             -PathPrefix 'Include/' -DestRoot $wtlInc -IncludeRegex '\.h$'
        if (-not $ok) { Say "  WTL 未完整获取，libPPUI 可能编译失败" Yellow }
    }
}

# ---------------------------------------------------------------------------
# 3. lunasvg —— 渲染 SVG 图标
# ---------------------------------------------------------------------------
if (-not $SkipLunasvg) {
    Step '3/4  lunasvg'

    $lsDir = Join-Path $third 'lunasvg'
    if ((Test-Path (Join-Path $lsDir 'include\lunasvg.h')) -and -not $Force) {
        Say "  已存在，跳过" DarkGray
    } else {
        New-Item -ItemType Directory -Force -Path $lsDir | Out-Null
        # 核心 + plutovg 光栅化器。examples 目录是示例程序，不要。
        $ok = Get-GithubTree -Owner 'sammycage' -Repo 'lunasvg' -Branch 'master' `
                             -PathPrefix '' -DestRoot $lsDir `
                             -IncludeRegex '^(include|source|plutovg)/(?!examples/).*\.(h|cpp|c)$'
        if (-not $ok) { Say "  lunasvg 未完整获取" Yellow }
    }
}

# ---------------------------------------------------------------------------
# 4. columns_ui-sdk —— CUI 面板接口
#
# 基础 SDK 里完全没有 CUI 接口，必须引这个外部 SDK。
#
# 【锁 commit，不要跟分支】它的签名对版本敏感（get_wnd() 现在是 const、
# create_or_transfer_window 收 const window_host_ptr& 之类），跟分支的话
# 上游一改我们某天就编不过。升级就改 $CuiSdkCommit 并重新编译验证。
# ---------------------------------------------------------------------------
if (-not $SkipCui) {
    Step '4/4  columns_ui-sdk (CUI)'

    $cuiDir = Join-Path $third 'columns_ui-sdk'
    if ((Test-Path (Join-Path $cuiDir 'window.h')) -and -not $Force) {
        Say "  已存在，跳过" DarkGray
    } else {
        New-Item -ItemType Directory -Force -Path $cuiDir | Out-Null
        # 只要仓库根目录的源码 —— docs/ 与 .github/ 用不上
        $ok = Get-GithubTree -Owner 'reupen' -Repo 'columns_ui-sdk' -Branch $CuiSdkCommit `
                             -PathPrefix '' -DestRoot $cuiDir -IncludeRegex '^[^/]+\.(h|cpp)$'
        if (-not $ok) { Say "  columns_ui-sdk 未完整获取，CUI 面板会编不过" Yellow }
    }
}

# ---------------------------------------------------------------------------
Step '结果'
foreach ($p in @(
    @{ n = 'foobar2000 SDK'; f = "$third\foobar2000\SDK\foobar2000_SDK.vcxproj" },
    @{ n = 'pfc';            f = "$third\pfc\pfc.vcxproj" },
    @{ n = 'libPPUI';        f = "$third\libPPUI\libPPUI.vcxproj" },
    @{ n = 'WTL';            f = "$third\WTL\Include\atlapp.h" },
    @{ n = 'lunasvg';        f = "$third\lunasvg\include\lunasvg.h" },
    @{ n = 'columns_ui-sdk'; f = "$third\columns_ui-sdk\window.h" }
)) {
    $present = Test-Path $p.f
    Say ("  {0,-16} {1}" -f $p.n, $(if ($present) { '就绪' } else { '缺失' })) $(if ($present) { 'Green' } else { 'Red' })
}

Say "`n下一步：.\build.ps1 -Install"
