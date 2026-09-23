# foobar2000 歌词插件 · 市场现状与开发工具链调研

> 调研时间：2026-09 ｜ 工作区：`D:\C#\lyric-plugin`（空目录，未初始化 git）
> 所有结论均标注来源；标注「未验证」的条目未找到一手证据，落地前需复核。

---

## 0. 结论速览（TL;DR）

**市场侧**

- 官方组件库 `lyrics` 标签下**只有 2 个组件**：Lyric Show Panel 3（0.6 / 2023-11-18）与 OpenLyrics（1.13 / 2026-01-17）。这个类目近乎真空。
- 真正的事实标准是**不在官方仓库里的 ESLyric**（`foo_uie_eslyric`，1.0.7.0 / 2026-09-13，GitHub 725 star），功能密度远超官方两位：面板+桌面歌词、Direct2D 渲染、CSS 式布局引擎、频谱可视化、**用户可写的 JS 歌词源与解析器**。
- 老牌 `foo_uie_lyrics3` 的源码从未公开，内置歌词源大面积失效——OpenLyrics 作者在 README 里明确说"这就是我又写一个的理由"。**歌词插件的头号死因不是渲染，是歌词源腐坏。**
- 空白区（机会）：中文逐字歌词的完整覆盖（KRC / QRC / YRC）、桌面歌词体验、现代渲染与动效、x64/ARM64 原生支持、以及**把"歌词源"做成可热更新、可用户扩展的插件形态**。

**工具链侧**

- 官方只有一条路：C++ + **foobar2000 SDK**（最新 `2026-09-17`，附带 VS2022/2026 工程文件）→ 编译出 `foo_xxx.dll` → 打包成 `.fb2k-component`（**本质就是 zip**）。
- 实测两个主流歌词组件的包结构完全一致：**根目录放 x86 DLL + `x64/` 子目录放 x64 DLL**，一个包同时吃 32/64 位。
- 实测工程配置：VS2022 / `v143` 工具集 / Windows SDK `10.0.19041.0`+ / C++17~C++20 / `/utf-8` / DLL 工程 / 链接 SDK 的 `shared-$(Platform).lib`。
- 三条可选路线见 §2，推荐 **C++ 原生面板做核心 + 歌词源用脚本/数据驱动**（理由见 §2.3）。

---

## 1. 市场现状

### 1.1 官方组件库：歌词类目只有两个组件

来源：<https://www.foobar2000.org/components/tag/lyrics>

| 组件 | 版本 / 最后更新 | 作者 | 平台 | 特点 |
|---|---|---|---|---|
| **Lyric Show Panel 3**（`foo_uie_lyrics3`） | 0.6 / **2023-11-18** | The vern | Win32 + Win64 | 基于 foo_uie_lyrics2 的老牌面板；DUI/CUI 双支持；0.6 的更新日志是「Removed dead sources, reinstated minilyrics」——**内容就是删失效源** |
| **OpenLyrics**（`foo_openlyrics`） | 1.13 / **2026-01-17** | jacquesh | Win32 + Win64 | MIT 开源；自带面板 + 多源在线下载；内置歌词编辑器、批量搜索、封面背景、外部窗口 |

要点：

- 官方仓库的 `lyrics` 类目**没有 ESLyric**。ESLyric 走 GitHub Releases + 论坛分发，说明官方仓库不是流量入口，也不是必须的发布渠道。
- Lyric Show Panel 3 从 2023 年 11 月后没有新版本，且**源码未公开**（OpenLyrics README 原话：*"the source for the plugin did not appear to be available anywhere online"*）。
- 官方仓库支持按平台筛选（x86 / x64 / arm64ec / macOS），两个歌词组件都标了「Windows 32-bit, Windows 64-bit」，即**均已提供 64 位版本**。

### 1.2 事实标准：ESLyric

来源：<https://github.com/ESLyric/release> ｜ wiki：<https://github.com/ESLyric/release/wiki> ｜ 脚本库：<https://github.com/ESLyric/scripts>

- 最新版本 **1.0.7.0（2026-09-13）**，仓库 725 star，仍在**高频迭代**（单次 release 说明就列出十几项布局引擎/渲染/接口改进）。
- 系统要求：**Windows 10 1607+ / foobar2000 v1.5+**。
- 功能面：DUI/CUI 双支持、Direct2D/DirectWrite 渲染、**面板歌词 + 桌面歌词 + 浮动歌词**、歌词搜索/内容过滤、可自定义布局、卡拉 OK 逐字显示、时间偏移微调（Shift+滚轮）、**外部 JS 面板控制接口**。
- 1.0.7.0 新增的方向很能说明"这个市场在卷什么"：CSS 风格的 `transition`/`transform`/`box-shadow`/`border-radius` 布局属性、封面主题色 `{dominant-color}`、频谱新增 `wave2` 绘制、歌词**弹性滚动 + 脱手拖拽调进度**。
- **架构关键点**：内置 **QuickJS-ng** JS 引擎，把两件事做成用户脚本——
  - **歌词源脚本**（`eslyric-data\scripts\searcher`）：导出 `getConfig(cfg)` / `getLyrics(meta, man)`，可用 `request()` 发 HTTP、`mxml` 解析 XML、`zlib` 压缩解压、`atob/btoa`。
  - **解析器脚本**（`eslyric-data\scripts\parser`）：把任意格式转成 ESLyric 认的 LRC。
  - 官方脚本库现有 **29 个歌词源** + **3 个解析器**。

### 1.3 竞品功能矩阵

| 能力 | ESLyric 1.0.7.0 | OpenLyrics 1.13 | Lyric Show Panel 3 0.6 |
|---|---|---|---|
| DUI / CUI | ✅ / ✅ | ✅ / ✅ | ✅ / ✅ |
| 面板歌词 | ✅ | ✅ | ✅ |
| 桌面/悬浮歌词 | ✅（面板+桌面+浮动，独立开关） | ✅（外部窗口） | ⚠️ 有限 |
| 逐字（卡拉OK） | ✅ | ✅（timed lyrics） | ✅ |
| 渲染方式 | Direct2D / DirectWrite | D2D/DWrite + WIC（工程里链了 d2d1/dwrite/windowscodecs） | GDI 系 |
| 布局自定义 | ✅ XML 布局引擎（CSS 式属性、动效） | ✅ 字体/配色/背景（封面+模糊+透明） | ⚠️ 有限 |
| 歌词汇编/编辑器 | ✅（带时间戳编辑器） | ✅（内置编辑器，支持时间戳） | ⚠️ |
| 批量下载 | ✅ | ✅（bulk search） | ⚠️ |
| 多源在线下载 | ✅ 29 个脚本源，可自写 | ✅ 内置 15 个源 | ⚠️ 多个源已失效 |
| 歌词源可扩展性 | ✅ **用户 JS 脚本，不用重编译** | ⚠️ 需改 C++ 重编译 | ❌ 需等作者 |
| 中文源（网易/QQ/酷狗） | ✅ netease / qqmusic / kugou + krc/qrc 解析器 | ✅ netease / qqmusic | ⚠️ 多数已失效 |
| 双语 / 参照行 | ✅ **同时间戳多行 = 参照行**（翻译、罗马音） | ✅（多语言歌词） | ⚠️ |
| 频谱可视化 | ✅ | ❌ | ❌ |
| 外部脚本接口 | ✅ 外部 JS 面板控制接口 | ❌ | ❌ |
| 开源 | ❌ 仅发布二进制 | ✅ MIT | ❌ |
| 在官方仓库 | ❌ | ✅ | ✅ |

### 1.4 从竞品逆推出的「机会点」

1. **歌词源腐坏是主要矛盾**。两个官方组件的历史都在反复"删失效源"。→ 架构上必须把源做成**可插拔 + 可热更新**（脚本或配置驱动），而不是编译进 DLL。
2. **中文逐字歌词是硬需求也是硬骨头**。网易 YRC、QQ QRC、酷狗 KRC 都要处理加密/压缩和逐字时间戳；ESLyric 用 `zlib` + `arraybuffer` + 独立解析器脚本解决。做中文市场绕不开。
3. **桌面歌词体验**（多屏、置顶、锁定、点击穿透、随播放高亮）是差异化战场，而不是面板本身。
4. **现代视觉**：D2D/DWrite + 动效 + 深色模式已经是 2026 年的及格线，ESLyric 已经把"弹性滚动、拖拽调进度、行放大过渡"做完了。
5. **平台覆盖**：foobar2000 已提供 32-bit / 64-bit / **ARM64EC** 三套安装包（当前稳定版 2.25.10）。实测官方仓库按 **Windows ARM** 筛选歌词类目的结果是 **"No components match the criteria"——目前没有任何原生 ARM 歌词组件**，ARM 版 foobar2000 只能靠**模拟运行 x64 组件**。这是一个完全空白的细分位（市场很小，但零竞争；且 ARM 设备上原生组件的性能/功耗优势真实存在）。

### 1.5 热度参考数据（GitHub API 实测）

| 项目 | Star | Fork | 最新发布 | 最新 release 资产下载量 |
|---|---|---|---|---|
| ESLyric（`ESLyric/release`） | 725 | 20 | 1.0.7.0 / 2026-09-13 | 480（发布后不久，属正常爬坡） |
| OpenLyrics（`jacquesh/foo_openlyrics`） | 601 | 50 | v1.13 / 2026-01-17 | **2541**（另 debug symbols 包 607） |

> 注：OpenLyrics 仓库最后一次 push 是 2026-09-20，但官方仓库页面仍显示 1.13 / 2026-01-17，存在滞后。

---

## 2. 技术路线选型

### 2.1 三条路线对比

| | **A. 原生 C++ 面板** | **B. WebView2 面板** | **C. JS 面板脚本** |
|---|---|---|---|
| 形态 | `foo_xxx.dll` 实现 DUI element / CUI panel | C++ 薄壳 + HTML/CSS/JS 界面 | 依赖宿主（JScript Panel 3 / JSplitter / SMP） |
| 语言 | C++17/20 + Win32 + D2D | C++（少）+ Web 技术栈 | JavaScript |
| 能力上限 | 最高（可做桌面歌词、Direct2D 自绘、任意 hook） | 高（UI 无上限，系统级能力受壳限制） | 低（受宿主暴露的 API 限制） |
| 上手成本 | 高（SDK + COM 式 service + 消息循环） | 中（会写前端就行） | 低 |
| 参考实现 | ESLyric、OpenLyrics、Lyric Show Panel 3 | `foo_ui_webview2`（内置 `lyrics` 命名空间 API）、`foo_uie_webview`、`foo_webview2` | `marc2k3/spider-monkey-panel-x64`、JSplitter |
| 分发 | `.fb2k-component` 直接装 | 同左（但依赖 WebView2 Runtime） | 脚本文件，用户手动放 |

### 2.2 WebView2 路线的现成弹药

`foo_ui_webview2`（<https://github.com/NereaFantasia/foo_ui_webview2>）值得单独记一笔：

- 提供 **200+ API / 20+ 命名空间**的双向 C++ ↔ JS 桥（BridgeCore），命名空间里**直接有 `lyrics`**；事件如 `playback:trackChanged`、`playback:time`。
- 带 **SMP 兼容层**（映射 35 个 SMP 回调 + `fb`/`plman` 包装），老 JS 脚本经验可迁移。
- 附 **npm TS SDK**（`foo-webview-sdk`）与 **MCP Server**（`npx foo-ui-webview2-mcp`，可让 AI 通过 CDP 驱动播放器、截图迭代主题）。
- 构建要求：VS2022 17.8+、**v145 工具集**、Windows SDK 10.0.22621+、WebView2 Runtime、C++20；主组件 GPL-3.0、`sdk/` MIT。
- ⚠️ 注意：主组件是 **GPL-3.0-or-later**，若你的插件要与它链接/派生，许可会传染；仅"在它里面跑一个 HTML 主题"则通常是另一回事，需要单独确认。

### 2.3 建议

> **核心用 C++（路线 A），歌词源层做成数据/脚本驱动。**

理由：

1. 桌面歌词、多屏置顶、D2D 自绘、点击穿透这些差异化能力，只有在原生层才做得舒服；WebView2 路线的窗口行为受宿主约束。
2. 路线 A 与 ESLyric 的护城河正面竞争；路线 B/C 更适合做**皮肤/主题**层面的差异化。
3. 无论哪条路线，**歌词源都不该编译进二进制**——这是市场给出的最贵的一课。
4. 折中方案：C++ 做核心（播放回调、歌词模型、渲染、桌面窗口）+ 内嵌脚本引擎（QuickJS-ng / duktape）跑歌词源脚本，与 ESLyric 同构。**注意**：直接抄 ESLyric 的脚本接口设计有兼容性收益（现成 29 个源脚本可复用），但要评估脚本来源的许可问题。

---

## 3. 开发工具链（可复制的最小环境）

### 3.1 官方 SDK

来源：<https://www.foobar2000.org/SDK>

- **最新版本：`2026-09-17`**（下载：`/downloads/SDK-2026-09-17.7z`），有 changelog。
- 附带工程文件：**Visual Studio 2022/2026**，以及 **Xcode 13-26**（macOS 侧）。
- 在线文档：<https://www.foobar2000.org/RTFM>
- **公开调试符号**：<https://www.foobar2000.org/PDB>（调试 foobar2000 本体时用得上）。
- 历史版本可回溯到 2011，老组件维护时能对齐 SDK 版本。

从真实组件的 `.vcxproj` 反推，SDK 内部结构大致是：

```
foo_SDK/
├── foobar2000/
│   ├── SDK/foobar2000_SDK.vcxproj              # 核心接口
│   ├── helpers/foobar2000_sdk_helpers.vcxproj  # 常用辅助
│   ├── foobar2000_component_client/…vcxproj    # 组件客户端/注册入口
│   └── shared/shared-x64.lib, shared-Win32.lib # 预编译库，按平台链接
├── pfc/pfc.vcxproj                             # Peter's Foundation Classes
└── libPPUI/libPPUI.vcxproj                     # UI 辅助
```

### 3.2 环境清单（三份真实工程交叉验证）

| 项目 | foo_openlyrics | foo_artwork | foo_ui_webview2 |
|---|---|---|---|
| IDE | VS2022 | VS2022 | VS2022 17.8+ |
| 工具集 | `v143` | `v143` | **`v145`** |
| Windows SDK | `10.0.19041.0` | `10.0.19041.0`+ | `10.0.22621`+ |
| C++ 标准 | `stdcpp20` | C++17 | C++20 |
| 编译开关 | `/utf-8`、PCH、`DynamicLibrary`、`CharacterSet=Unicode` | `/utf-8` | — |
| 平台配置 | Win32 + x64 各 4 个配置 | x64（及 32 位） | x64 + x86 打包 |
| 输出 | `foo_openlyrics.dll` | `foo_artwork.dll` | `.fb2k-component` |

> **结论**：最小可用组合 = **VS2022 + v143 工具集 + Windows 10 SDK 10.0.19041 或更高 + C++17 起**。想用最新工具集（v145 / VS2026）也可以，SDK 已提供对应工程。

### 3.3 依赖库（按需要加）

- **WTL 10**（`WTL10_10320_Release`）—— 原生 Win32 UI 几乎必用。
- **Columns UI SDK**（`columns_ui-sdk-7.0.0-beta.2`）—— 要做 CUI 面板就得加；很多项目直接用它来顺带拿到完整 foobar2000 SDK。
- **HTTP**：libcurl 8.16 + nghttp2（OpenLyrics 路线，用 CMake 3.31.6 单独构建）或 WinHTTP/WinINet（零依赖路线）。
- **解析**：cJSON、pugixml 1.12.1、tidy-html5 5.8.0（HTML 抓取清洗）、nlohmann/json。
- **图像**：WIC（`windowscodecs.lib`）+ D2D/DWrite；stb 系列可选。
- 实测链接的系统库（OpenLyrics）：`bcrypt, Crypt32, d2d1, d3d11, dwrite, dxguid, Iphlpapi, Secur32, windowscodecs, Ws2_32`。

### 3.4 构建与打包

构建（以 foo_artwork 的脚本为例，需在 VS 开发者命令行里跑）：

```bat
msbuild foo_SDK\foobar2000\SDK\foobar2000_SDK.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
msbuild foo_SDK\pfc\pfc.vcxproj                    /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
msbuild foo_xxx.vcxproj                            /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

打包：**`.fb2k-component` 就是改名的 zip**。实测拆包结果：

```
foo_uie_eslyric_1.0.7.0.fb2k-component
├── foo_uie_eslyric.dll          4,569,600   ← x86
└── x64/foo_uie_eslyric.dll      6,092,288   ← x64

foo_openlyrics-v1.13.fb2k-component
├── foo_openlyrics.dll           2,265,088   ← x86
└── x64/foo_openlyrics.dll       2,634,752   ← x64
```

→ **打包约定：根目录放 32 位 DLL，`x64/` 子目录放 64 位 DLL，一个包通吃。** 用户双击即可安装（或 Preferences → Components → Install）。

另：OpenLyrics 把调试符号**单独**发一个 `foo_openlyrics-v1.13-with_debug_symbols.zip`（12.9 MB），主包保持 1.8 MB。这是值得照抄的分发习惯。

### 3.5 调试

- 编译 Debug 配置 → 附加到 `foobar2000.exe` 进程；把 DLL 放进 `<profile>\user-components\foo_xxx\`。
- 输出用 SDK 的控制台接口 + `OutputDebugString`（DebugView 可看）。
- 需要看播放器内部调用栈时，下载官方 PDB（<https://www.foobar2000.org/PDB>）。
- ⚠️ 组件崩溃会拖垮整个播放器，Debug 阶段建议单独开一个便携版 profile 隔离。

### 3.6 本机环境实测（2026-09）

| 组件 | 实测结果 | 评估 |
|---|---|---|
| Visual Studio | **Professional 2026**（18.8.12021.73，装在 `D:\VS studio`） | ✅ 可用 |
| VS Build Tools | **2026**（18.7.11919.86，`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`） | ✅ 可做纯命令行构建 |
| MSVC 工具集 | **14.51.36231 → `v145`** | ⚠️ **没有 `v143`** |
| Windows SDK | **仅 `10.0.26100.0`** | ⚠️ **没有 `10.0.19041`** |
| CMake | 4.4.2 | ✅（OpenLyrics 构建 libcurl 需 ≥ 3.31） |
| git | 2.54.0 | ✅ |
| 7-Zip | **未安装** | ⚠️ SDK 分发的是 `.7z` |

**坑 1：工具集/SDK 版本错位。** 网上现成工程（`foo_openlyrics`、`foo_artwork`）都把 `PlatformToolset` 写死为 `v143`、`WindowsTargetPlatformVersion` 写死为 `10.0.19041.0`。本机只有 v145 + SDK 26100，**直接打开这些工程会报"找不到工具集 / 找不到 SDK"**。两条路：

- (a) 用 VS Installer 补装 v143 工具集 + 10.0.19041 SDK（多下 2~3 GB）；
- (b) **把工程重定向到 v145 + 26100**（推荐）——官方 SDK `2026-09-17` 附带的工程就是给 VS2022/2026 的，新建工程默认也对得上，跟官方保持一致。

**坑 2：没有 7-Zip。** 官方 SDK 分发格式是 `.7z`。最省事是装一个（`winget install 7zip.7zip`）；Windows 自带的 `tar.exe`（bsdtar / libarchive）理论上支持读 7z，但**未实测**。

---

## 4. 架构参考：从竞品逆推

### 4.1 歌词数据模型的"事实标准"

ESLyric 的解析器脚本要求最终产出以下**任一**形式（这等价于业界通用中间格式）：

1. **标准 LRC**：`[00:00.00]line 1`
2. **增强型 LRC（逐字）**：`[mm:ss.xx] <mm:ss.xx> word1 <mm:ss.xx> word2 …`
3. **参照行（双语/翻译/罗马音）**：**同一时间戳的多行**，第一行为原始歌词，其余为参照行，逐字效果只作用于第一行：

```
[00:01.00]Hello
[00:01.00]你好
[00:01.00]表示问候
[00:02.00]<00:02.00>World<00:03.00>
[00:02.00]世界
```

→ 建议自己的内部模型就按 **行 →（时间戳 + 若干"轨道"）+ 词级时间戳** 来设计，参照行天然表达为多轨道，双语是它的特例。

### 4.2 歌词源清单（现成可对照/复用的）

**ESLyric 脚本库实测清单**（<https://github.com/ESLyric/scripts>）：

- `searcher/`（29 个）：absolutelyrics、azlyrics、bandcamp、chartlyrics、darklyrics、elyrics、genius、**kugou**、letras、lololyrics、**lrclib**、lyricalnonsense(+en)、lyricsmania、lyricsmode、lyricstranslate、metallum、minilyrics、musixmatch、**netease**、netease_en、oldielyrics、plyrics、**qqmusic**、songlyrics、songmeanings、stlyrics、ttplayer_obsolete
- `parser/`（3 个）：**krc.js（酷狗）**、**qrc.js（QQ 音乐）**、srt.js

**OpenLyrics 源码里的内置源**（`src/sources/`）：azlyricscom、bandcamp、darklyrics、id3tag、letras、lrclib、lyricfind、lyricsify、metalarchives、musixmatch、netease、qqmusic、songlyrics、genius、localfiles。

→ 两个项目**独立收敛到同一批源**：**LRCLIB 是首选公共源**，中文三大家是网易/QQ/酷狗，欧美是 Genius/AZLyrics/Musixmatch/Letras。

### 4.3 值得抄的两个机制

1. **歌词源的脚本化**（ESLyric）：源写在 `scripts/searcher/*.js`，解析器写在 `scripts/parser/*.js`，网站改版时**改脚本即可，不动 DLL**。这是应对"源腐坏"的唯一有效解法。
2. **媒体库歌词索引**（OpenLyrics 的 `lyric_metadb_index_client`）：给整个媒体库建歌词索引，可以在不播放的情况下批量搜索/检查歌词。做"批量下载歌词"功能时是基础设施。

---

## 5. 风险与待确认项

| # | 事项 | 状态 |
|---|---|---|
| 1 | **SDK 许可条款**：是否禁止商业销售组件、是否禁止再分发 SDK | 未验证（需读 SDK 包内 license 原文） |
| 2 | 官方组件仓库的收录条件与流程 | 待确认（详见后续文档） |
| 3 | ARM64EC 是否需要单独的工程配置/工具集 | **部分确认**：官方仓库歌词类目 ARM 筛选为空（无原生 ARM 歌词组件），ARM 版 foobar2000 通过模拟兼容 x64 组件；但"如何编译出 ARM64EC 组件"仍需查 SDK/VS 配置 |
| 4 | 各歌词源的 API 可用性与限流、ToS/版权风险 | 待确认（详见后续文档） |
| 5 | 复用 ESLyric 脚本接口设计的许可影响（ESLyric 未开源） | 需评估 |
| 6 | WebView2 路线与 GPL-3.0 组件的链接边界 | 需法务确认 |
| 7 | KRC/QRC/YRC 的加密/压缩细节与解密实现 | 待确认（详见后续文档） |

---

## 6. 下一步建议

1. **先定路线**（§2.3）：C++ 原生 + 脚本化歌词源，还是 WebView2 快速验证？
2. **做一个"能装进 foobar2000 并显示正在播放曲目"的最小骨架**：DUI element + CUI panel + 播放回调 + 读本地 `.lrc`。这一步打通即证明工具链闭环。
3. **再补歌词源层**：先 LRCLIB（无 key、结构简单），再网易/QQ（逐字）。
4. **最后做差异化**：桌面歌词 / 逐字渲染 / 多源聚合与缓存。

---

## 附：来源

- foobar2000 SDK 下载与版本：<https://www.foobar2000.org/SDK>
- foobar2000 Windows 版与平台（32/64/ARM64EC）：<https://www.foobar2000.org/windows>
- 官方组件库 · lyrics 标签：<https://www.foobar2000.org/components/tag/lyrics>
- 官方组件库 · lyrics × Windows ARM（实测为空）：<https://www.foobar2000.org/components/tag/lyrics/system/arm64ec>
- Lyric Show Panel 3 组件页：<https://www.foobar2000.org/components/view/foo_uie_lyrics3>
- OpenLyrics 仓库与 README：<https://github.com/jacquesh/foo_openlyrics>
- ESLyric 发布仓库：<https://github.com/ESLyric/release> ｜ wiki：<https://github.com/ESLyric/release/wiki>
- ESLyric 脚本库（29 源 + 3 解析器）：<https://github.com/ESLyric/scripts>
- foo_ui_webview2（WebView2 路线）：<https://github.com/NereaFantasia/foo_ui_webview2>
- foo_artwork（构建流程样例）：<https://github.com/jame25/foo_artwork>
