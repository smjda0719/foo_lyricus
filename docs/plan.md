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
| **M2** | 播放控制 | **组合面板**：歌词 2~3 行在上、控制键在下（播放/暂停、上下曲、进度条、音量）。控件一律自绘（标准控件无法参与分层合成，见 D-012）；命中测试按区域区分（见 D-013）；播放事件走主线程消息，不可在全局回调里直接刷界面 |
| **M3** | LRC 解析器 | 读同目录同名 `.lrc`，解析为内部模型；**用 `Battleplan Obliteration.lrc` 当夹具**，覆盖 3 位小数时间戳 / CRLF / 空行 / 末行无换行 |
| **M3** | LRC 解析器 | ✅ **已完成** 编码识别（UTF-8/UTF-16/GB18030）、毫秒精度时间戳、CRLF、空行、末行无换行、`[offset:]`、一行多时间戳；同目录同名自动匹配（`file://` URL 需 `filesystem::g_get_native_path` 转换）；手动指定**按曲目记住** |
| **M4** | 歌词面板 | DUI + CUI 面板渲染歌词，当前行高亮、平滑滚动 |
| **M5** | 扩展 | 逐字（增强 LRC）、LRCLIB 在线源、缓存、双语参照行 |

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
├── docs/                       # 调研与决策
├── downloads/                  # SDK / WTL 原始压缩包
├── 3rdparty/
│   ├── foobar2000/             # 官方 SDK（已解压）
│   ├── libPPUI/
│   ├── pfc/
│   └── WTL10_10320_Release/    # 待获取
├── src/
│   ├── main.cpp                # 组件入口、版本声明
│   ├── playback_state.*        # play_callback 订阅 + 状态缓存
│   ├── control_window.*        # 独立操作面板（M1/M2）
│   ├── backdrop.*              # Mica/Acrylic 封装 + 降级
│   ├── lyric_parser.*          # LRC 解析（M3）
│   ├── lyrics_panel.*          # DUI/CUI 歌词面板（M4）
│   └── config.*                # 配置持久化
├── build/
│   ├── lyricus.sln
│   └── foo_lyricus.vcxproj
├── tests/                      # 解析器单测
└── Battleplan Obliteration.lrc # 测试夹具
```

---

## 待解决

| # | 事项 | 状态 |
|---|---|---|
| 1 | 获取 WTL 10 | 进行中 |
| 2 | 用 `foo_sample` 的哪几个文件当骨架 | 待定（`main.cpp` / `initquit.cpp` / `ui_element.cpp` 是主要参考） |
| 3 | 独立窗口是"组件内自建窗口"还是"DUI/CUI 面板 + 撕下" | 倾向自建顶层窗口 |
| 4 | 是否同时出 x86 | 推荐出（成本低） |
