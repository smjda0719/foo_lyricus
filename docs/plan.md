# Lyricus 开发计划

## 组件标识

| 项 | 值 |
|---|---|
| 组件名 | **Lyricus** |
| DLL | **`foo_lyricus.dll`** |
| 目标宿主 | foobar2000 2.x（x64 优先，x86 同步出） |
| 系统要求 | Windows 11 22H2 (22621) 及以上才能用上模糊效果；更低版本走降级 |

---

## 两个交付物

### 1. 歌词面板

DUI element + CUI panel 双支持，用 `uie_shim_panel` 思路一个实现出两套。

### 2. 独立操作面板（副屏用）

**动机**：foobar2000 主界面在副屏上不容易看清（布局小、字小、离得远）。

- 独立顶层窗口，置顶（`HWND_TOPMOST`），可拖到任意显示器
- **Mica / Acrylic 背景**（半透明毛玻璃），可切换"不透明 / Mica / Acrylic"
- 播放控制：播放/暂停、上一首/下一首、进度条（可拖拽 seek）、音量、静音
- 显示：曲名 / 艺术家 / 专辑 / 封面 / 当前时间
- 位置和外观**持久化**（记住上次在哪个显示器、哪个位置）
- 可选：点击穿透、无边框自定义标题栏、圆角

---

## 里程碑

| # | 目标 | 验收标准 |
|---|---|---|
| **M0** | 工程跑通 | `foo_lyricus.dll` 能被 foobar2000 加载，在组件列表里显示 "Lyricus" 和版本号 |
| **M1** | 独立窗口 + 透明 | ✅ **已完成（2026-09-22）** 置顶无边框窗口；**分层窗口渲染的深灰半透明**（alpha 215/255），文字实心白；可拖动、位置持久化、Esc 隐藏、菜单开关、开机自动恢复。Mica/Acrylic 路径保留但**与 GDI 绘制不兼容**（见 D-009） |
| **M2** | 播放控制 | ✅ **已完成** 组合面板：歌词 3 行在上、控制条在下（播放/暂停、上下曲、可拖拽进度条、时间、音量条、静音、滚轮调音量）。控件全自绘（D-012）；命中测试按区域区分（D-013）；SVG 图标见 D-016 |
| **M3** | LRC 解析器 | ✅ **已完成并单测覆盖（39 项全绿）** 编码识别（UTF-8 有无 BOM / UTF-16LE BOM / GB18030 兜底）、1/2/3 位小数各自正确缩放、一行多时间戳、CRLF、空行、末行无换行、`[offset:]` 正负、畸形输入不崩；同目录同名自动匹配（`file://` URL 需 `filesystem::g_get_native_path` 转换）；手动指定**按曲目记住**（D-014） |
| **M4a** | DUI 元素 | ✅ **已完成** `ui_element_impl_withpopup` + `ui_element_subclass_playback_information`；自带子窗口 + **双缓冲绘制**（单缓冲下"擦背景"和"不残留"是矛盾的）；歌词行数**按可用高度自适应** |
| **M4b** | CUI 面板 | ✅ **已完成并真机验证（2026-09-23）** `uie::window` + `uie::window_factory`。**首次运行即通过**，日志证据：`CUI: 面板窗口已创建 hwnd=...`、`CUI: 主题刷新 background=FFFFFF text=000000 current=D77800 dim=595959 dpi=96 宿主字体=有`、`CUI 显示设置变更 -> ...`。即窗口创建、CUI 配色服务、宿主字体、显示设置轮询**四条链路全部打通**。配色/字体走 `common_callback`（D-020），SDK 版本关系见 D-021 |
| **M5** | 配置项 | ✅ **已完成并真机验证（2026-09-23）** 7 个 `advconfig` 条目挂在「高级 → Lyricus」下：字号 / 行数 / 当前行位置 / 额外歌词目录 / 模糊匹配 / 标签匹配 / 在线查询。**改动即时生效**：三种宿主各自在 250ms 定时器里轮询比对（advconfig 没有变更通知，轮询是唯一可靠办法） |
| **M5b** | 显示设置快捷入口 | ✅ **已完成并验证** `视图 → Lyricus` 多了三条「点一下换下一档」的命令（字号 / 行数 / 当前行位置），和「高级」读写同一份存储。真机：字号 100%→125% 时歌词亮带 **34→44 px**；位置 50%→60% 时该行中心 **y=126→142**。布局数学另有离线单测：`span=1` 恰好 4 条带、`span=3` 恰好 8 条带、`ratio` 25/50/75 对应当前行 y=183/322/462 |
| **M6a** | 歌词搜索智能化 | ✅ **已完成并验证（2026-09-23）** 五条策略打分：精确(100) / 去前缀(90,85) / **去音轨号(88)** / 标签构造(80) / 模糊(76-95) + extraDir 减 5。模糊用字符二元组 Dice + 包含率双闸门，阈值偏保守 —— **找错歌词比找不到更糟**。**离线单测 25 项全绿**（`tests\harness\run.ps1`，直接跑真实源码，不用播放器） |
| **M5c** | 外观可配置 | ✅ **已完成（2026-09-23）** 首选项页 `显示 → Lyricus`：6 个色块（点开系统取色器）+ 不透明度滑块 + 恢复默认。菜单那条改成**通透度四档循环**（100/95/85/70%）。**只管浮动面板** —— DUI/CUI 继续跟随宿主主题（见 D-030） |
| **M6b** | LRCLIB 在线源 | ✅ **已完成并真机验证（2026-09-23）** WinHTTP + 后台线程 + `fb2k::inMainThread` 回主线程。缓存区分「确实没有」（`.miss`，7 天过期）与「查询失败」（不写缓存）。**503 重试路径在真实 503 上验证通过**；缓存命中与 `.meta` 往返也验证过。见 D-024 / D-027 |
| **M6c** | 其余扩展 | 逐字（增强 LRC）、双语参照行 |

> **轮询而非通知**：`advconfig` 的改动没有任何回调机制，三种宿主只能每帧比三个 int。
> 代价可忽略，换来的是「用户在首选项里改完抬头就看见效果」。
> 注意**不要**改用 `advconfig_*_factory_cached` —— 它的缓存永不失效，见 D-019 附近的说明。

> 顺序理由：M1+M2 最快产出**能立刻用上**的东西（副屏操作面板），M3+M4 才是原始目标的歌词显示。

---

## 技术选型要点

| 项 | 选择 | 说明 |
|---|---|---|
| 工具集 | **v145** + Windows SDK **10.0.26100** | 实测：SDK 自带 `.vcxproj` 写死 v143，需重定向；核心库 `pfc` / `foobar2000_SDK` / `component_client` / `shared` 用 v145 **编译通过** |
| 构建 | MSBuild（`D:\VS studio\MSBuild\Current\Bin\MSBuild.exe`） | 用 Professional 那套，因为 **ATL 只装在它下面** |
| UI 库 | ATL（已装） + **WTL 10**（需自备，SDK 不带） | `libPPUI` 和 `sdk_helpers` 都依赖 `atlapp.h` |
| 渲染 | Direct2D + DirectWrite | 与 ESLyric 同路线；工程需链 `d2d1 / dwrite / dxguid / windowscodecs` |
| 模糊 | **`DwmSetWindowAttribute` + `DWMWA_SYSTEMBACKDROP_TYPE`** | 见下 |
| HTTP（后续） | WinHTTP 或 libcurl | 先不引入，M5 再说 |

### 模糊方案：不用 Windows App SDK

微软官方 "Apply Mica in Win32" 文档走的是 Windows App SDK（`MicaController` + `Compositor` + C++/WinRT + DispatcherQueue + 用户侧 Windows App Runtime）。**对注入 foobar2000 进程的组件来说代价太大**。

改用 DWM 系统背景：

```cpp
// Windows SDK 26100 的 dwmapi.h 已有这些常量，无需魔数
BOOL dark = TRUE;
DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

int backdrop = DWMSBT_TRANSIENTWINDOW;   // 3 = Desktop Acrylic（毛玻璃）
DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
```

| 值 | 含义 |
|---|---|
| `DWMSBT_MAINWINDOW` (2) | Mica |
| `DWMSBT_TRANSIENTWINDOW` (3) | **Desktop Acrylic**（毛玻璃，透出后面的窗口） |
| `DWMSBT_TABBEDWINDOW` (4) | Mica Alt |

- **最低系统**：Windows 11 Build 22621（本机 22631 ✓）
- 效果绘制在**整个窗口边界之下**，正好是独立面板需要的
- **降级路径**（Win10 及更早）：`SetWindowCompositionAttribute` + `ACCENT_ENABLE_ACRYLICBLURBEHIND`（未公开 API；SDK 的 `libPPUI/DarkMode.cpp` 已有同族调用先例）
- ⚠️ **已知问题**：Win11 上 Acrylic 窗口拖动时可能卡顿（微软承认），缓解办法是拖动期间临时切不透明

### 线程模型（硬约束）

`playback_control` 头文件顶部明确写着：**所有方法只能在主线程调用，且不能在全局回调（如 playlist callback）里调用**，否则"什么也不做或抛异常"。

→ 窗口/按钮的事件处理、以及任何跨线程请求，都必须经 `main_thread_callback` 或窗口消息回到主线程再调 `playback_control`。

---

## 目录结构（规划）

```
D:\C#\lyric-plugin\
├── docs/                       # 调研、计划、决策记录（decisions.md 按编号倒序累积）
├── downloads/                  # SDK / WTL / Columns UI 原始压缩包
├── 3rdparty/
│   ├── foobar2000/             # 官方 SDK 2026-09-17
│   ├── columns_ui-sdk/         # CUI 面板接口，锁 commit 69972e36（独立仓库，见 D-021）
│   ├── libPPUI/
│   ├── lunasvg/                # SVG 光栅化（含 plutovg）
│   └── WTL10_10320_Release/    # SDK 不带 WTL，需自备
├── src/
│   ├── main.cpp                # 组件入口、版本声明
│   ├── stdafx.h                # 预编译头（<helpers/foobar2000+atl.h>）
│   ├── config.*                # cfg_var 配置项 + 设置项的结构声明
│   ├── settings.cpp            # advconfig 工厂 + Get/Set（存储是 fb2k::configStore）
│   ├── debug_log.*             # 写到 DLL 同级的 lyricus-debug.log
│   ├── lyric.*                 # LRC 解析、编码识别、宽窄转换
│   ├── lyric_search.*          # 多策略歌词搜索（M6a）
│   ├── playback_state.*        # play_callback 订阅 + 状态缓存 + 手动绑定表
│   ├── lyrics_view.*           # 三种宿主共用的绘制层
│   ├── control_window.*        # 独立操作面板（M1/M2）
│   ├── dui_element.cpp         # DUI 元素（M4a）
│   ├── cui_panel.cpp           # CUI 面板（M4b）
│   ├── svg_icon.*              # SVG 图标加载、着色、翻转
│   ├── menu.cpp                # 视图 → Lyricus 子菜单
│   └── initquit.cpp            # 启动恢复 / 退出保存
├── resources/                  # SVG 图标，随 DLL 一起部署到同级的 resources\
├── tests/
│   ├── harness/                # ★ 纯逻辑离线单测台（跑真实源码，不用播放器）
│   │   ├── run.ps1             #   拷源码 + 套 shim → cl.exe 编译 → 跑四组
│   │   ├── test_search.cpp     #   搜索算法          30 项
│   │   ├── test_lyric.cpp      #   LRC 解析器        39 项
│   │   ├── test_online.cpp     #   在线歌词 JSON     54 项
│   │   ├── test_view.cpp       #   绘制层布局        15 项
│   │   └── shim/               #   stdafx.h / SDK\cfg_var.h / playback_state.h
│   └── fixtures/               # 静音 WAV 夹具（真机联调用，见 D-024/D-027）
│       ├── local/              #   「厂牌 - 曲名.wav」配「曲名.lrc」
│       ├── online/             #   周杰伦 - 晴天（对齐 LRCLIB 时长）
│       ├── nomatch/            #   确实没有歌词
│       └── wrongsong/          #   同名但是别人的歌（验时长闸门）
├── tools/
│   ├── setup.ps1               # 拉依赖（断点续传 + 重试）
│   ├── hot-install.ps1         # ★ 热安装：不关播放器就换 DLL（见 D-026）
│   ├── watch-install.ps1       # 等播放器退出 → 装 → 重启（需要自动重启时用）
│   └── capture-window.ps1      # DPI 感知的窗口截图，用于验证渲染
├── build/foo_lyricus.vcxproj
└── bin/x64/Release/foo_lyricus.dll
```

> ⚠️ **手动安装路径是 `user-components-x64\<name>\`**，不是 `user-components\` —— 见 D-007。
> 装错了 foobar2000 会**一声不吭地不加载**，组件列表里什么都没有。

---

## 待解决

| # | 事项 | 状态 |
|---|---|---|
| 1 | **CUI 面板真机验证** | Columns UI 3.7.0 已装好，**等用户切过去实测**。这是那份 638 行代码第一次真跑 |
| 2 | 显示设置的实测手感 | 默认值（100% / 自适应 / 50%）是**忠实保留**旧观感，不是新调的。等用户反馈 |
| 3 | 模糊匹配阈值 0.80 / 0.85 | 经验值，**没在真实曲库标定过**。误配了就调高 |
| 4 | LRCLIB 在线源 | 进行中。**已用真实数据探底**（D-024）：主流中文歌覆盖好，同人曲/OST 基本没有；`/api/search` 会张冠李戴，必须核对时长 |
| 5 | 逐字歌词（增强 LRC 的 `<mm:ss.xx>`） | 未开始。数据模型预留了扩展位 |
| 6 | 双语参照行 | **数据已就绪**：网易云的 `tlyric` 字段按时间戳对齐（实测《夜に駆ける》60 行翻译）。用户 2026-09-24 决定本轮不做 —— 面板只有 460×150、可见 2~3 行，完整双语会把可见行数砍半。解析层已留着该字段 |
| 7 | `playlist.svg` 没接进 UI | 图标文件在，暂时没用上 |
| 9 | **多接几个歌词源来对比** | 用户 2026-09-24 提的方向。现状：本地 → 网易云 → LRCLIB **串行兜底**。接第三个源之前要先想清楚两件事：<br>① **串行的代价** —— 每多一个源，最坏情况就多一次网络往返（而且要等它彻底失败才走下一个）。源多了要改成**并行查询 + 按置信度取优**。<br>② **每个源都是一份新的怪癖** —— 这一轮光是网易云一个源就踩了：非官方接口、限流（HTTP 200 + code 405）、繁简写法、翻唱首条、制作人员名单。再加三个就是三套。<br>候选：QQ音乐 / 酷狗 / 酷我（都是非官方接口）；Musixmatch / Genius（有正规 API 但要 key 和配额）。<br>**架构上已经就位**：`TryNetEase` 就是源的形状（搜索 → 核验 → 取词 → 剥名单），共用的部分都已经抽出来了 —— `MakeTitleCandidates` / `ArtistNamesOverlap` / `IsPlaceholderTag` / 两道软闸 / 缓存（键与来源无关）/ `.miss` 语义（所有源都没命中才写） |
| 8 | **被顶掉的在线查询没有取消机制** | **用户 2026-09-24 拍板：暂不做，先用一阵。** 现状见 D-032 末条 —— 切歌后旧查询会一直重试到 ~25s 才被丢弃（日志里实测白跑 26 秒）。快速连点切歌时它们会堆起来、抢主线程。修法清楚（`HttpGet` 重试循环里有检查点，加一个取消令牌），**权衡是取消会丢掉本该写进缓存的结果**。等用户实测觉得还卡再动 |

### 已知小问题（不影响使用，记着）

- **面板太矮时「行数」设置看不出效果** —— 不是 bug，是几何限制。
  默认面板 460x150 逻辑像素，歌词区实际只有 ~50 逻辑像素高。
  实测：字号 125% + 当前行位置 60% 时，当前行占掉 44 px 文字 + 20 px 行距，
  下一行需要 34+12=46 px，而控制条上沿只剩 ~11 px → 一行就是极限。
  **想看到多行：把面板拉高，或把字号调回 100%。**
  容易误认为「设置没生效」。
- ~~`ResetToDefaults()` 用的是系统 DPI 而不是窗口所在显示器的 DPI~~
  **复查后确认不是 bug，别改。** `ResetToDefaults()` 同时把位置重置成 `-1,-1`，
  而 `ComputeInitialRect()` 也是用**系统 DPI** 缩放、居中到**主显示器**工作区 ——
  两者口径一致。改成"窗口所在显示器的 DPI"反而会造出
  「尺寸按副屏算、位置在主屏」的错配。
- 分层渲染每帧重建 DIB —— 频率是 250ms 一次，实测无感，但不够讲究
- `BlendColor` 在 `dui_element.cpp` 和 `cui_panel.cpp` 各有一份，该提到公共层
- 组件目录里会攒下 `foo_lyricus.dll.old`（热安装的回滚备份），不影响加载，可随手删
- `dui_element.cpp` / `cui_panel.cpp` 缓存的 `LOGFONT` 目前**没人消费**
  （`LyricsViewTheme` 没有字体字段，渲染层自己按 dpi 造字体）。

  ⚠️ 真要接的时候注意：`LOGFONTA` 60 字节 / `LOGFONTW` 92 字节，
  跨 DLL 传裸结构体对 `<CharacterSet>` 设置有硬依赖 —— 见 D-019。
