# 决策记录

> 记录已经拍板的技术决策与理由，避免后面反复。改动请追加新条目，不要覆盖旧结论。

---

## D-001 技术路线：原生 C++ 面板

- **决定**：走官方 SDK 的 C++ 路线，实现 DUI element + CUI panel。
- **理由**：桌面歌词、多屏置顶、Direct2D 自绘、点击穿透这些差异化能力只有在原生层才做得舒服；WebView2 路线的窗口行为受宿主（`foo_ui_webview2`）约束，且该组件为 GPL-3.0。
- **否决的备选**：WebView2 混合方案、JS 面板脚本（JScript Panel 3 / JSplitter / SMP）。

## D-002 工具集与 SDK 版本：`v145` + Windows SDK `10.0.26100`

- **决定**：不补装 v143 工具集和 10.0.19041 SDK，直接用本机现有的 **VS Professional 2026（18.8.12021.73，`v145` / MSVC 14.51.36231）+ Windows SDK 10.0.26100.0**。
- **理由**：本机只有这一套；省下 2~3 GB 下载。

### 实测核实（2026-09，SDK 已下载解压到 `3rdparty/`）

官方 SDK `2026-09-17` 的 **7 个 `.vcxproj` 全部**写着：

```
PlatformToolset             = v143
WindowsTargetPlatformVersion = 10.0
LanguageStandard            = stdcpp20
```

- `WindowsTargetPlatformVersion=10.0` 表示"用已安装的最新 10.x SDK" → 本机自动解析到 `10.0.26100.0`，**无需改动**；
- **`v143` 本机没有**（只有 `v145` / MSVC 14.51.36231），**必须改**。

### ⚠️ 修正此前的错误判断

之前根据官网那句 "Included project files are for Visual Studio 2022/2026" 推测"官方工程面向 VS2026、可直接用"——**不成立**。
官网那句话的意思是"工程能在 VS2022/2026 里打开"，**前提是装了 v143 工具集组件**。既然决定不装，就要把 toolset 重定向到 `v145`。

**影响**：官方 SDK 自带工程、以及网上 clone 的现成工程（`foo_openlyrics`、`foo_artwork`）**都要重定向**。
后者还把 `WindowsTargetPlatformVersion` 写死成 `10.0.19041.0`，这个也得改成 `10.0`（跟随最新）或 `10.0.26100.0`。

## D-003 用途：自用

- **决定**：这是自己用的插件，不做商业化分发。
- **影响**：
  - SDK 许可中关于"能否销售组件"的条款**不再是阻塞项**，"2015 版 SDK 的 Usage restrictions" 无需再追。
  - 官方组件仓库的收录流程也不需要研究，GitHub Releases / 本地安装足够。
  - ⚠️ 若日后改变主意要公开发布二进制，**回头读 SDK 包内的 `sdk-license.txt` 原文**再定。

## D-004 打包结构：沿用官方约定

- **决定**：`.fb2k-component` 按官方约定打包。
- **约定**：
  - 根目录放 **32 位** `foo_xxx.dll`，`x64/` 子目录放 **64 位** `foo_xxx.dll`；
  - **包内除 DLL 外不放任何其他文件**（已知事故：vgmstream 因多放一个 README，导致 64 位 DLL 装不到位、64 位宿主完全不加载组件）；
  - PDB 调试符号**另打一个 zip**。
- **依据**：实测 ESLyric 1.0.7.0 与 OpenLyrics v1.13 的包结构一致；另见 `research-02` §5。

## D-005 窗口模糊：用 DWM 系统背景，不引入 Windows App SDK

- **决定**：独立操作面板的 Mica/Acrylic 走 **DWM 系统背景**（`DwmSetWindowAttribute` + `DWMWA_SYSTEMBACKDROP_TYPE`）。
- **理由**：微软官方 "Apply Mica in Win32" 文档要求 Windows App SDK（`MicaController` + `Compositor` + C++/WinRT + `DispatcherQueue` + 用户侧 Windows App Runtime 可再发行包）。对一个注入 foobar2000 进程的组件来说，初始化 WinRT apartment 和引入运行时依赖**代价过大、风险高**。
- **实测确认**：`DWM_SYSTEMBACKDROP_TYPE`（`dwmapi.h`）**最低支持 Windows 11 Build 22621**，本机 22631 ✓。
  取值：`DWMSBT_MAINWINDOW`=2（Mica）、`DWMSBT_TRANSIENTWINDOW`=3（**Desktop Acrylic**，毛玻璃）、`DWMSBT_TABBEDWINDOW`=4（Mica Alt）。效果绘制在**整个窗口边界之下**，正合独立面板所需。
- **额外优势**：Windows SDK 26100 的 `dwmapi.h` 已定义这些常量（含 `DWMWA_USE_IMMERSIVE_DARK_MODE`=20），**无需魔数**。
- **降级**：Win10 及更早走 `SetWindowCompositionAttribute` + `ACCENT_ENABLE_ACRYLICBLURBEHIND`（未公开 API；SDK 的 `libPPUI/DarkMode.cpp` 已有同族调用先例）。
- **已知问题**：Win11 上 Acrylic 窗口**拖动时可能卡顿**（微软已承认），缓解办法是拖动期间临时切不透明。

## D-006 构建环境：MSBuild 用 Professional 那一套

- **决定**：用 `D:\VS studio\MSBuild\Current\Bin\MSBuild.exe`（VS2026 Professional）。
- **理由**：**ATL 只装在 Professional 下**（`D:\VS studio\VC\Tools\MSVC\14.51.36231\atlmfc` 存在），BuildTools 那套（`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231`）**没有 atlmfc**。libPPUI 和 sdk_helpers 都需要 ATL。
- **另需自备 WTL**：SDK 不自带 `atlapp.h`，`libPPUI` 与 `foobar2000_sdk_helpers` 编译会失败（实测 `error C1083`）。

## D-007 手动安装路径：64 位宿主用 `user-components-x64`

- **决定**：手动安装组件时，目标路径是
  `%APPDATA%\foobar2000-v2\user-components-x64\<组件名>\<dll>`
  **不是** `user-components\<组件名>\<dll>`。
- **实测依据（铁证）**：foobar2000 2.25.9 (x64) 于 2026-09-22 23:17:18 启动时**自己创建了** `user-components-x64` 与 `user-components-x64\pending` 两个目录；而 `user-components` 目录**此前根本不存在**（是我们手工建的）。
- **静态证据**：`foobar2000.exe` 内含字符串 `user-components` 与 **`user-components-`（带连字符，说明是前缀）**，以及 `Could not list pending components directory`。即目录名 = `user-components` + 架构后缀。
- **推论**：32 位宿主用 `user-components`，64 位宿主用 `user-components-x64`（同一个 profile 可被两种架构共用而不打架）。
- ⚠️ **打包结构不受影响**：`.fb2k-component` 内部仍是「根目录 x86 DLL + `x64/` 子目录 x64 DLL」，由播放器安装器自行挑选架构，见 D-004。

## D-008 网络：GitHub 走本地 8902 代理

- **决定**：从 GitHub 下载（源码包、Release 资产等）时使用代理 `http://127.0.0.1:8902`。
- **格式**：`curl.exe -x http://127.0.0.1:8902 -L -o <输出> <url>`
- **实例验证**：直连下载 WTL 得到 2,023,776 字节的**截断包**（7-Zip 报 `Unexpected end of archive`）；走代理得到 **5,810,309 字节**的完整包，`7z t` 校验 `Everything is Ok`。
- **注意**：`web_fetch` / `web_search` 工具走的是另一条链路，**无法指定该代理**；只有本机 curl / PowerShell 发起的请求能用。

## D-009 绝不对客户区使用 `DwmExtendFrameIntoClientArea(-1)`

- **症状**：面板一片空白（只有背景，没有文字）。**按一下 `PrintScreen` 文字会闪现，取消后立刻消失**；任何截屏工具都能"看到"内容，肉眼看不到。
- **原因**：把整个客户区变成 DWM "边框"之后，客户区里的 GDI 绘制内容**不会被 DWM 呈现到屏幕上**。截屏动作会强制重新合成，所以只有截屏时能看到。
- **教训**：**"截图能看见、肉眼看不见"是这个 bug 的特征**，不是截图工具的问题。排查时如果双方观测不一致，先怀疑"呈现"而不是"绘制"。
- **结论**：DWM 系统背景材质（`DWMWA_SYSTEMBACKDROP_TYPE`）与普通 GDI 绘制**不兼容**。该 API 调用全部返回 `S_OK`，但"接受设置"不等于"正确显示"——不要以 HRESULT 判断成败。

## D-010 面板渲染改用分层窗口（`UpdateLayeredWindow`）

- **决定**：面板默认走**分层窗口 + 逐像素 alpha** 渲染：自建 32bpp DIB（`BITMAPV5HEADER` + `BI_BITFIELDS`），手工按 BGRA 写入底色与 alpha，再用 GDI 画文字（**GDI 只改 RGB，不碰 alpha**），最后 `UpdateLayeredWindow` 提交。
- **理由**：
  1. 不依赖 DWM 材质，绕开 D-009 的坑；
  2. 解决"文字背后出现黑色方块"——不自己铺底时，GDI 往透明表面写字会把文字范围写成不透明黑底；
  3. 真透明可控（当前整体 alpha 215/255），后续要做"背景半透明 + 文字全不透明"只需分别写 alpha。
- **代价**：没有真模糊（毛玻璃）。要做真模糊需要 DirectComposition + D2D，属于后续里程碑。
- **注意**：`UpdateLayeredWindow` 与 `SetLayeredWindowAttributes` **互斥**，不能同时用；分层模式下画面不经过 `WM_PAINT`，必须在 `WM_MOVE`/`WM_SIZE`/显示时主动重提交。
- **已知待优化**：目前每次重绘都新建 DIB 并逐像素填充，`WM_MOVE` 期间会很浪费。M2 之前应改为缓存 DIB。

## D-011 分层渲染必须同时做「alpha 修正」和「预乘 alpha」

- **症状**：面板成功透明了，但**文字也跟着透明**——发灰、发糊、像蒙了一层。
- **两个独立原因，只修一个不够**：
  1. **GDI 只写 RGB，从不碰 alpha 通道** → 文字像素的 alpha 仍是底色的 215；
  2. **`UpdateLayeredWindow(ULW_ALPHA)` 要求源位图是预乘 alpha 的**（RGB 已乘 `alpha/255`），直接写非预乘值会让整张图偏暗。
- **修法**：画完文字后扫一遍像素 —— 与底色不同的即文字（含抗锯齿边缘），把 alpha 提到 255；然后整图做一次预乘。
- **代价**：抗锯齿边缘被当作实心像素处理，边缘会略硬。要更精细得改用 GDI+ 或 Direct2D 绘制文字（它们会正确写 alpha）。
- **实现位置**：`src/control_window.cpp` 的 `RenderLayered()`。

## D-012 面板不用系统标准控件，一律自绘

- **决定**：M2 的播放控制按钮、进度条、音量条**全部自绘**，不使用 `BUTTON` / `msctls_trackbar32` 等标准控件。
- **来源**：用户经验——标准控件在透明底上表现可能很差。
- **架构层面的硬约束（比"表现差"更根本）**：分层窗口通过 `UpdateLayeredWindow` 提交位图，**子窗口控件无法参与这种合成**。标准控件会各自往窗口表面绘制，与逐像素 alpha 冲突，表现为控件区域变成不透明块或黑块。**所以自绘不是更优选，而是唯一可行解。**
- **与 D-011 同源**：本质都是「GDI 绘制与 alpha 合成不匹配」的同一类问题。
- **对 M2 的具体影响**：
  1. 所有控件画进**同一张 DIB**，复用 `DrawTextContent` 那一套渲染路径；
  2. **命中测试要自己算矩形**。M1 里整个客户区返回 `HTCAPTION`（所以到处都能拖），M2 必须按控件区域区分，否则点按钮会变成拖窗口；
  3. 悬停 / 按下状态与重绘要自己实现。
- **附带好处**：外观完全可控、风格统一，也顺便给了 M1 遗留的「抗锯齿边缘略硬」一个统一处理的地方（同一张 DIB 上可以用更精细的方式生成 alpha）。

---

## D-013 面板信息密度：歌词 2~3 行，控制键放歌词下方

- **来源**：用户建议。
- **背景**：面板默认 460x150 逻辑像素（200% 缩放下 920x300 物理）。M1 纯歌词能显示 4 行左右；M2 要加播放控制，空间立刻不够。
- **决定**：
  1. 歌词**只保留 2~3 行**（当前行 ±1），不再用 ±2；
  2. 做成**组合面板**——歌词在上、控制按键在下（播放/暂停、上一首/下一首、进度条、音量）；
  3. 底部预留条改作控制条，歌词来源信息收缩或移入提示。
- **理由**：副屏是远距离观看，行数太多反而看不清重点；控制键是高频操作，值得占固定位置。
- **对 M2 的具体影响**：
  - `kSpan` 由 2 改为 1，并考虑做成可配置（`cfg_int`）；
  - `DrawTextContent` 要明确划分三个纵向区域：**头部 / 歌词区 / 控制条**，各占固定高度，互不侵占；
  - 控制条的**命中测试必须按区域区分** —— M1 里整个客户区都返回 `HTCAPTION`（所以到处都能拖窗口），加了按钮之后不改的话，点按钮会变成拖窗口（见 D-012）。

## D-014 手动指定的歌词「按曲目记住」，不是单一键值对

- **来源**：用户从日志里发现的 bug。
- **症状**：给 A 指定的歌词 → 播放 B → 换回 A，**忘了**。日志证据：
  `手动歌词绑定的是别的曲目，解除绑定`，之后回到 A 时只剩自动匹配。
- **原因**：早期实现只存**一对**（曲目, 路径），一旦播放别的歌就整条丢弃。
- **决定**：改成一张「曲目 URL → 歌词路径」的表，存在 `cfg_manual_lyric_map`，
  **纯文本格式**（每条一行，`URL <TAB> 路径`），上限 128 条。
  用最朴素的格式是为了出问题时能直接在配置文件里看和改。
- **特例**：URL 为空的那条表示「选文件时没在播放」，会被下一首播放的曲目**收养**。
- **「恢复自动匹配歌词」**：只清除**当前曲目**的记录（外加待定记录），不再一刀切。
- **教训**：这是我的**设计不合理**，而不是实现 bug —— 代码完全按我写的逻辑工作。
  盯着代码看不出来，得从「用户预期」的角度审。

## D-015 重绘判据用「状态代次」，不用字段比对

- **症状**：换曲后画面**停留在上一首的歌词**上。日志明确显示已换曲且「未找到可用歌词」，
  但屏幕没更新。
- **原因**：重绘判据是 `if (idx != m_currentLine)`。换曲时 `m_currentLine` 被设为 `npos`，
  而空歌词算出来也是 `npos` —— **两者相等，被判定为「没变化」，于是不重绘**。
- **决定**：引入 `PlaybackState::Revision()` 状态代次，换曲 / 加载歌词 / 清除歌词时自增；
  窗口每 tick 比对，不一致就重绘。
- **理由**：字段比对要枚举所有「应该重绘」的情形，漏一个就出这种静默 bug；
  代次只要在状态变更处自增一次，天然覆盖。
- **发现方式**：**日志说状态对了，截图说画面没跟上** —— 两者分叉才暴露出来。
  只信其中一个都会漏掉。

---

## 待定

| # | 事项 | 说明 |
|---|---|---|
| 1 | 是否同时出 32 位 | 成本很低（同一工程两个 Platform），建议出 |
| 2 | 歌词源机制 | 脚本化（内嵌 JS 引擎）还是纯 C++ 内置源 —— 等 `research-03` 歌词源/格式调研 |
| 3 | 是否复用 `uie_shim_panel<T>` 思路 | 强烈建议，一个实现出 DUI+CUI 两套面板 |
| 4 | 仓库初始化 | `git init` + VS `.gitignore`（`build/`、`intermediate/`、`.vs/`、SDK 目录不入库） |
