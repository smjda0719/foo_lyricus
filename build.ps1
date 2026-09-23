<#
  Lyricus 构建脚本

  用法：
    .\build.ps1                    # 编 SDK 依赖 + 组件 + 打包 .fb2k-component
    .\build.ps1 -SkipSdkDeps       # 跳过 SDK 依赖（已经编过时用，快很多）
    .\build.ps1 -Platform Win32    # 编 32 位（需先有 Win32 的 SDK 库）

  背景（为什么这么写）：
    * 工具集必须是 v145 —— 本机只有 VS2026，没有 v143
    * MSBuild 必须用 Professional 那一套 —— ATL 只装在它下面
      （BuildTools 那套的 VC\Tools\MSVC\<ver>\atlmfc 不存在）
    * SDK 自带工程不能直接 ProjectReference —— 它们的包含目录没带 WTL
      所以这里先单独把它们编成 .lib，再由 foo_lyricus.vcxproj 直接链接
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')]     [string]$Platform      = 'x64',
    [switch]$SkipSdkDeps,
    [switch]$NoPackage,
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$sdkDir  = Join-Path $root '3rdparty\foobar2000'
$msbuild = 'D:\VS studio\MSBuild\Current\Bin\MSBuild.exe'
$sz      = 'C:\Program Files\7-Zip\7z.exe'

function Write-Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "  $text" -ForegroundColor Green }

if (-not (Test-Path $msbuild)) {
    throw "找不到 MSBuild：$msbuild`n请确认 VS2026 Professional 的安装路径（ATL 只装在它下面）。"
}

# --------------------------------------------------------------------------
# 1. SDK 依赖库
# --------------------------------------------------------------------------
if (-not $SkipSdkDeps) {
    Write-Step "构建 SDK 依赖库 ($Platform / $Configuration / v145)"

    $sdkProjects = @(
        @{ name = 'pfc';                          path = "$sdkDir\..\pfc\pfc.vcxproj" }
        @{ name = 'foobar2000_SDK';               path = "$sdkDir\SDK\foobar2000_SDK.vcxproj" }
        @{ name = 'foobar2000_component_client';  path = "$sdkDir\foobar2000_component_client\foobar2000_component_client.vcxproj" }
        @{ name = 'shared';                       path = "$sdkDir\shared\shared.vcxproj" }
    )

    foreach ($p in $sdkProjects) {
        Write-Host "  -> $($p.name)"
        & $msbuild $p.path /p:Configuration=$Configuration /p:Platform=$Platform `
                   /p:PlatformToolset=v145 /v:minimal /nologo /m | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "构建 $($p.name) 失败（exit $LASTEXITCODE）" }
        Write-Ok "ok"
    }
}

# --------------------------------------------------------------------------
# 2. 组件本体
# --------------------------------------------------------------------------
Write-Step "构建 foo_lyricus ($Platform / $Configuration)"

$proj = Join-Path $root 'build\foo_lyricus.vcxproj'
& $msbuild $proj /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal /nologo /m
if ($LASTEXITCODE -ne 0) { throw "构建 foo_lyricus 失败（exit $LASTEXITCODE）" }

$dll = Join-Path $root "bin\$Platform\$Configuration\foo_lyricus.dll"
if (-not (Test-Path $dll)) { throw "构建成功但找不到产物：$dll" }
Write-Ok "产物：$dll  ($([math]::Round((Get-Item $dll).Length/1KB,1)) KB)"

# --------------------------------------------------------------------------
# 3. 打包 .fb2k-component
#    约定：根目录放 32 位 DLL，x64\ 子目录放 64 位 DLL；包内不放任何其他文件
#    （已知事故：多放一个 README 会导致 64 位 DLL 装不到位、组件完全不加载）
# --------------------------------------------------------------------------
if (-not $NoPackage) {
    Write-Step "打包 .fb2k-component"

    $stage   = Join-Path $root 'intermediate\package'
    $pkgDll  = Join-Path $root "bin\$Platform\$Configuration\foo_lyricus.dll"
    $pkgOut  = Join-Path $root "bin\foo_lyricus.fb2k-component"

    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    if ($Platform -eq 'Win32') {
        Copy-Item $pkgDll (Join-Path $stage 'foo_lyricus.dll')
    } else {
        New-Item -ItemType Directory -Force -Path (Join-Path $stage 'x64') | Out-Null
        Copy-Item $pkgDll (Join-Path $stage 'x64\foo_lyricus.dll')
    }

    if (Test-Path $pkgOut) { Remove-Item $pkgOut -Force }
    if (Test-Path $sz) {
        & $sz a -tzip -mx=9 $pkgOut (Join-Path $stage '*') | Out-Null
    } else {
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $pkgOut -Force
    }
    Write-Ok "包：$pkgOut"
    Write-Host "  注意：当前只含 $Platform 一种架构。要出双架构包，先用 -Platform Win32 编一次并合并。" -ForegroundColor Yellow
}

# --------------------------------------------------------------------------
# 4. 安装到 foobar2000 配置目录
#
#    目录名带架构后缀：64 位宿主读 user-components-x64，32 位宿主读 user-components。
#    放错目录的表现是「组件完全不出现，也没有任何加载失败记录」——见 docs/decisions.md D-007。
# --------------------------------------------------------------------------
if ($Install) {
    Write-Step "安装到 foobar2000"

    $profileDir = Join-Path $env:APPDATA 'foobar2000-v2'
    if (-not (Test-Path $profileDir)) { throw "找不到 foobar2000 配置目录：$profileDir" }

    $archDir = if ($Platform -eq 'Win32') { 'user-components' } else { 'user-components-x64' }
    $destDir = Join-Path $profileDir "$archDir\foo_lyricus"
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    Copy-Item $dll (Join-Path $destDir 'foo_lyricus.dll') -Force
    Write-Ok "$destDir\foo_lyricus.dll"

    # SVG 图标：运行期从 DLL 同级的 resources\ 加载，必须一起部署
    $resSrc = Join-Path $root 'resources'
    if (Test-Path $resSrc) {
        $resDst = Join-Path $destDir 'resources'
        New-Item -ItemType Directory -Force -Path $resDst | Out-Null
        Copy-Item (Join-Path $resSrc '*.svg') $resDst -Force
        Write-Ok "$resDst  ($((Get-ChildItem $resDst -Filter *.svg).Count) 个 SVG)"
    }

    if (Get-Process foobar2000 -ErrorAction SilentlyContinue) {
        Write-Host "  注意：foobar2000 正在运行，需要重启才会加载新版本。" -ForegroundColor Yellow
    }
}

Write-Host "`n完成。" -ForegroundColor Green
