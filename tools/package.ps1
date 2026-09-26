<#
.SYNOPSIS
    把构建产物打成 foobar2000 能直接安装的 .fb2k-component。

.DESCRIPTION
    组件包就是个 zip，但**目录结构有硬要求**（见 D-004）：

        foo_lyricus.fb2k-component
        |- foo_lyricus.dll           <- 32 位（放在根目录）
        +- x64/
           +- foo_lyricus.dll        <- 64 位

    foobar2000 按**自己进程的位数**挑：32 位播放器读根目录那份，64 位读 x64/。
    ⚠️ 装错了 foobar2000 **一声不吭地不加载** —— 组件列表里什么都没有，
       也不报错。所以这个结构不是"风格问题"，错了就是白的。

    ⚠️ 本工程目前**只构建 x64**（build\foo_lyricus.vcxproj 里只有 x64 平台配置），
       所以打出来的包只有 x64/ 一份。32 位播放器装了不会生效 ——
       脚本会在结尾明确提示这一点，而不是假装包是完整的。
       要补 32 位，先给 vcxproj 加 Win32 平台配置、构建出
       bin\Win32\Release\foo_lyricus.dll，脚本会自动带上它。

.PARAMETER OutDir
    输出目录，默认 bin\。

.PARAMETER Force
    即使包比源 DLL 新也重新打（默认会跳过并提示"已经是最新的"）。

.EXAMPLE
    .\tools\package.ps1
    日常用这个。已经是最新的就什么都不做。

.EXAMPLE
    .\tools\package.ps1 -Force -OutDir dist
    强制重打并放到 dist\。
#>

[CmdletBinding()]
param(
    [string]$OutDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot

if (-not $OutDir) { $OutDir = Join-Path $root 'bin' }
if (-not (Test-Path $OutDir)) { [void](New-Item -ItemType Directory -Path $OutDir -Force) }
$OutDir = (Resolve-Path $OutDir).Path

$outPath = Join-Path $OutDir 'foo_lyricus.fb2k-component'

# 找构建产物。32 位是可选的 —— 找不到就只打 64 位。
$dll64 = Join-Path $root 'bin\x64\Release\foo_lyricus.dll'
$dll32 = Join-Path $root 'bin\Win32\Release\foo_lyricus.dll'

if (-not (Test-Path $dll64)) {
    throw "找不到 64 位构建产物：$dll64`n先构建一次：MSBuild build\foo_lyricus.vcxproj /p:Configuration=Release /p:Platform=x64"
}
$has32 = Test-Path $dll32

Write-Host "`n=== 打包 Lyricus 组件 ===" -ForegroundColor Cyan
Write-Host "  64 位: $([math]::Round((Get-Item $dll64).Length/1KB,1)) KB  $((Get-Item $dll64).LastWriteTime.ToString('MM-dd HH:mm'))"
if ($has32) {
    Write-Host "  32 位: $([math]::Round((Get-Item $dll32).Length/1KB,1)) KB  $((Get-Item $dll32).LastWriteTime.ToString('MM-dd HH:mm'))"
} else {
    Write-Host "  32 位: （没有构建，包里不会含 root 那份）" -ForegroundColor DarkYellow
}

# 包已经比源新就不用重打。
# ★ 这一步是**这个脚本存在的主要理由** —— 上一个包是手工打的，
#   结果陈旧了四天没人发现（里面还是 39KB 的骨架版，而当时 DLL 已经 916KB）。
if ((Test-Path $outPath) -and -not $Force) {
    $pkgTime = (Get-Item $outPath).LastWriteTime
    $newest  = @(Get-Item $dll64) + $(if ($has32) { @(Get-Item $dll32) } else { @() }) |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($pkgTime -ge $newest.LastWriteTime) {
        Write-Host "`n  包已经是最新的（$($pkgTime.ToString('MM-dd HH:mm'))），跳过。" -ForegroundColor Green
        Write-Host "  要强制重打加 -Force。`n"
        exit 0
    }
    Write-Host "  包陈旧（$($pkgTime.ToString('MM-dd HH:mm'))），重新打。" -ForegroundColor Yellow
}

# 用临时目录摆好结构再压缩。
# 为什么不用 Compress-Archive 直接从源文件列表打：那样控制不了 entry 名字里的
# 路径分隔符，Windows 上容易进去反斜杠而 Linux/macOS 解压时把它当普通字符。
# 摆好目录再按目录压缩，entry 名由 API 生成，稳定。
$stage = Join-Path $env:TEMP ("lyricus-pkg-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    [void](New-Item -ItemType Directory -Path (Join-Path $stage 'x64') -Force)
    Copy-Item $dll64 (Join-Path $stage 'x64\foo_lyricus.dll')
    if ($has32) { Copy-Item $dll32 (Join-Path $stage 'foo_lyricus.dll') }

    if (Test-Path $outPath) { Remove-Item $outPath -Force }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    # includeBaseDirectory = false：stage 这一层本身不进包
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stage, $outPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)

    Write-Host "`n  已生成: $outPath" -ForegroundColor Green

    # 打完之后把结构列出来 —— 包错了是**静默失效**，必须眼见为实。
    Write-Host "`n  包内容:" -ForegroundColor Cyan
    $zip = [System.IO.Compression.ZipFile]::OpenRead($outPath)
    try {
        foreach ($e in $zip.Entries) {
            Write-Host ("    {0,-40} {1,8} 字节" -f $e.FullName, $e.Length)
        }
    } finally { $zip.Dispose() }

    # 自检：必须恰好是"根可选 + x64 必有"这两个位置
    $zip = [System.IO.Compression.ZipFile]::OpenRead($outPath)
    try {
        $entries = @($zip.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
    } finally { $zip.Dispose() }

    $ok = ($entries -contains 'x64/foo_lyricus.dll')
    Write-Host ""
    if ($ok) { Write-Host "  ✓ x64/foo_lyricus.dll 在正确的位置" -ForegroundColor Green }
    else     { Write-Host "  ✗ 缺少 x64/foo_lyricus.dll —— 64 位播放器不会加载！" -ForegroundColor Red }

    if (-not $has32) {
        Write-Host "  ⚠ 包里没有根目录的 foo_lyricus.dll —— 32 位 foobar2000 装了不会有任何反应。" -ForegroundColor DarkYellow
        Write-Host "    （本工程只构建 x64；要支持 32 位需先给 vcxproj 加 Win32 平台配置）" -ForegroundColor DarkYellow
    }
    Write-Host ""
} finally {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
