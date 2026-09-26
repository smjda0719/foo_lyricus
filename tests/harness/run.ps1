<#
  纯逻辑离线单测台 —— 搜索算法 + LRC 解析器。

  做什么
  ------
  在临时目录里各放一份**逐字节复制**的真实源码，配上 tests/harness/shim/ 里的
  替身头文件，用 cl.exe 编成控制台程序并运行。

  为什么这么搭
  ------------
  * `FindLyricFile()` 是**对文件系统的纯函数**，`LyricDocument::Parse()` 是
    **对字节数组的纯函数** —— 两者都不需要播放器、不需要 foobar2000 宿主，
    直接调就能验证。比"造静音夹具让播放器去播"快几个数量级，也不打扰用户。
  * 这两个 .cpp 唯一的 SDK 依赖来自 stdafx.h 和 config.h 里的 <SDK/cfg_var.h>。
    而头文件用引号包含时**优先在包含者所在目录**找，所以替身必须和源码同目录 ——
    这就是"拷到临时目录再编"而不是直接 -I 的原因。拷的是逐字节副本，
    不存在"测的是另一份代码"的问题。
  * config.h / lyric.h / lyric_search.h / debug_log.h 用的都是**真实那份**，
    只有 stdafx.h 和 <SDK/cfg_var.h> 被替换。

  用法
  ----
    .\run.ps1             # 跑全部
    .\run.ps1 -Suite lyric # 只跑某一组
#>

[CmdletBinding()]
param(
    [ValidateSet('all', 'search', 'lyric', 'order', 'online', 'view', 'cbar', 'prefs', 'wheel', 'hint', 'preset', 'bench')] [string]$Suite = 'all'
)

$ErrorActionPreference = 'Stop'

$harness = $PSScriptRoot
$root    = Split-Path -Parent (Split-Path -Parent $harness)
$src     = Join-Path $root 'src'
$workBase = Join-Path $env:TEMP 'lyricus-unit-tests'

$vcvars = 'D:\VS studio\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    Write-Host "找不到 vcvars64.bat：$vcvars" -ForegroundColor Red
    exit 1
}

# 三组各自独立编成 exe。
#
# 为什么不能合成一个：三边都要提供 lyricus::DebugLog，搜索那组还要自己实现
# WideToUtf8，符号会撞。
#
# 注意 online 那组的 compile 里**没有** online_lyric.cpp ——
# 它是由 test_online.cpp 直接 #include 进去的（为了让匿名命名空间里的
# JSON 解析函数可见）。再单独编一遍就会重复定义。
$suites = @(
    @{ key='search'; name='搜索算法';      compile=@('test_search.cpp','lyric_search.cpp');            extra=@();                        shims=@();                  libs='' },
    @{ key='lyric';  name='LRC 解析器';    compile=@('test_lyric.cpp','lyric.cpp');                    extra=@();                        shims=@();                  libs='' },
    # 歌词源顺序：纯逻辑 + 配置串容错，不碰 SDK（见 source_order.h 的说明）。
    @{ key='order';  name='歌词源顺序';    compile=@('test_source_order.cpp','source_order.cpp');                         extra=@('source_order.h');        shims=@();                  libs='' },
    # online 组要**额外**编 lyric_search.cpp：网易云候选核验复用了那边的
    # NormalizeLyricStem。不加就会在链接期报 LNK2019 修饰名看不懂。
    @{ key='online'; name='在线歌词 JSON'; compile=@('test_online.cpp','lyric.cpp','lyric_search.cpp','source_order.cpp'); extra=@('online_lyric.cpp','online_lyric.h','source_order.h'); shims=@();          libs='winhttp.lib crypt32.lib' },
    # 绘制层这组要用**替身** playback_state.h 覆盖真实那份：
    # 真实那份要读 metadb / playback_control，而绘制层只用几个只读访问器。
    @{ key='view';   name='绘制层布局';    compile=@('test_view.cpp','lyrics_view.cpp','scroll_anim.cpp','lyric.cpp'); extra=@('lyrics_view.h','scroll_anim.h'); shims=@('playback_state.h'); libs='gdi32.lib user32.lib' },
    # 控制条布局：纯函数，只要 windows.h 的 RECT / MulDiv，不碰 SDK。
    # 2026-09-26 加的 —— 这段逻辑那天改了两轮（进度条被挤没、空矩形画鬼影），
    # 两轮都只能靠"算一遍 + 截图看"，所以抽出来让断言钉住。
    @{ key='cbar';   name='控制条布局';    compile=@('test_control_bar.cpp','control_bar_layout.cpp'); extra=@('control_bar_layout.h'); shims=@(); libs='' },
    # 首选项页：全自绘，布局和配色都是纯函数（只依赖 windows.h）。
    # 2026-09-26 重做该页时加的 —— 自绘最典型的 bug 是元素叠在一起，
    # 而那恰好是"给定尺寸 -> 一组矩形"能精确钉住的东西。
    @{ key='prefs';  name='首选项页布局';  compile=@('test_prefs_layout.cpp','prefs_layout.cpp','color_util.cpp'); extra=@('prefs_layout.h','color_util.h'); shims=@(); libs='' },
    # 色环取色器：HSV 换算 + 几何 + 命中测试，全是纯数学。
    # 最要紧的是"坐标 <-> 颜色"两个方向必须互逆 —— 点红色就该选中红色。
    @{ key='wheel';  name='色环取色器';    compile=@('test_color_wheel.cpp','color_wheel.cpp');         extra=@('color_wheel.h');        shims=@(); libs='' },
    # 歌词线索表：文本表 <-> 内存表、增删查改、512 上限。原先整个 folder_hint.cpp
    # 因为 include 了 config.h / online_lyric.h 一行都测不了，2026-09-26 把纯逻辑
    # 切进 folder_hint_table.cpp。要 lyric.cpp 是为了 WideToUtf8 / Utf8ToWide。
    @{ key='hint';   name='歌词线索表';    compile=@('test_folder_hint.cpp','folder_hint_table.cpp','lyric.cpp'); extra=@('folder_hint_table.h'); shims=@(); libs='' },
    # 外观预设：文本表解析、增删改、单条导入导出。难点全在**容错**上 ——
    # 缺字段、多字段、坏值、BOM、CRLF、前后空行，这些在界面上试不出来。
    @{ key='preset'; name='外观预设';      compile=@('test_preset.cpp','preset.cpp','lyric.cpp'); extra=@('preset.h'); shims=@(); libs='' },
    # bench 不是测试，是**基准**：它只打印耗时，不判通过与否。
    # 所以默认的 all 会跳过它（不能让"性能数字"影响单测的通过/失败），
    # 要用就显式 -Suite bench。
    @{ key='bench';  name='搜索成本基准';  compile=@('bench_search.cpp','lyric_search.cpp');           extra=@();                        shims=@();                  libs='' }
)

# 真实头文件（各组都要）
# 每次都要复制过去的 src 头。
#
# ⚠️ 这里漏一个头，表现是**另一个组突然编不过** —— 因为 config.h 现在
#    include 了 preset.h（D-088），而 search / online 那两个组只用
#    config.h，于是它们会因为找不到 preset.h 而 C1083。
#    所以：**给 config.h 加新 include 时，记得回来看看这里**。
$headers = @('config.h', 'preset.h', 'lyric.h', 'lyric_search.h', 'debug_log.h')

$totalPass = 0
$totalFail = 0
$failedSuites = @()

foreach ($s in $suites) {
    if ($Suite -ne 'all' -and $Suite -ne $s.key) { continue }
    # all 跳过基准组：它没有"通过/失败"的概念，跑起来还慢（要建 3500 个文件）
    if ($Suite -eq 'all' -and $s.key -eq 'bench') { continue }

    Write-Host ''
    Write-Host ("################ {0} ################" -f $s.name) -ForegroundColor Cyan

    $work = Join-Path $workBase $s.key
    if (Test-Path $work) { Remove-Item $work -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Join-Path $work 'SDK') | Out-Null

    # 真实源码，逐字节复制
    foreach ($f in ($headers + $s.extra)) {
        $p = Join-Path $src $f
        if (-not (Test-Path $p)) { Write-Host "缺少 $p" -ForegroundColor Red; exit 1 }
        Copy-Item $p $work -Force
    }
    # 替身
    Copy-Item (Join-Path $harness 'shim\stdafx.h')      $work -Force
    Copy-Item (Join-Path $harness 'shim\SDK\cfg_var.h') (Join-Path $work 'SDK') -Force
    foreach ($f in $s.shims) {
        Copy-Item (Join-Path $harness "shim\$f") $work -Force
    }
    # 夹具头（由真实响应自动生成，见 kugou_fixtures.h）。
    # 它住在 harness 目录而不是 src —— extra 只从 src 找，所以单独复制一次。
    $fixtureHeader = Join-Path $harness 'kugou_fixtures.h'
    if (Test-Path $fixtureHeader) { Copy-Item $fixtureHeader $work -Force }
    # compile 列表里既有 harness 目录的测试文件，也有 src 目录的源码 —— 两头都找一遍
    foreach ($f in $s.compile) {
        $hp = Join-Path $harness $f
        $sp = Join-Path $src $f
        if     (Test-Path $hp) { Copy-Item $hp $work -Force }
        elseif (Test-Path $sp) { Copy-Item $sp $work -Force }
        else   { Write-Host "找不到 $f" -ForegroundColor Red; exit 1 }
    }

    Write-Host '  编译...' -ForegroundColor DarkGray
    $srcList = ($s.compile | ForEach-Object { $_ }) -join ' '
    $linkPart = if ($s.libs) { " /link $($s.libs)" } else { '' }
    $cmd = @(
        "call `"$vcvars`" >nul 2>&1",
        "cd /d `"$work`"",
        "cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I `"$work`" /Fe:`"$work\test.exe`" $srcList$linkPart"
    ) -join ' && '

    & cmd.exe /c $cmd | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  编译失败 —— 重新单独编一次看错误：' -ForegroundColor Red
        & cmd.exe /c $cmd
        $failedSuites += $s.name
        continue
    }

    & (Join-Path $work 'test.exe')
    if ($LASTEXITCODE -ne 0) { $failedSuites += $s.name }
}

Write-Host ''
Write-Host '========================================' -ForegroundColor Cyan
if ($failedSuites.Count -eq 0) {
    Write-Host '*** 全部通过 ***' -ForegroundColor Green
    exit 0
} else {
    Write-Host ("*** 未通过: {0} ***" -f ($failedSuites -join ', ')) -ForegroundColor Red
    exit 1
}
