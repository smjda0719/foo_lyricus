# foobar2000 组件技术解剖与发布流程

> 来源：子代理联网调研（2026-09）。标注「未验证」的条目未取得一手证据，落地前需复核。
> 配套文档：`research-01-market-and-toolchain.md`

---

## 1. 组件的本质

**`foo_*.dll` 就是一个普通 Windows DLL**，靠一个导出函数与宿主握手：

```cpp
extern "C" __declspec(dllexport) foobar2000_client* _cdecl
foobar2000_get_interface(foobar2000_api* api, HINSTANCE self);
```

（证据：SDK 内 `component_client.cpp`）

**注册不是写注册表**，而是 DLL 内的**静态 service 工厂链**：

- `get_service_list()` 返回 `service_factory_base::__internal__list`；
- 宿主遍历这个链，逐个实例化；
- 所有扩展点都是 `service_base` 派生 + `service_ptr_t<T>` 持有 + `service_factory_single_t<T>` / `FB2K_SERVICE_FACTORY` 声明。

**ABI 版本判定**：`FOOBAR2000_CLIENT_VERSION = FOOBAR2000_TARGET_VERSION`，另有 `FOOBAR2000_CLIENT_VERSION_COMPATIBLE = 72`（2015 SDK 的值）。宿主据此决定加载不加载。

---

## 2. 歌词插件真正需要的接口

| 需求 | 接口 | 备注 |
|---|---|---|
| 跟随播放进度 | `play_callback::on_playback_time(double)` | **每秒回调一次，只在主线程** |
| 换曲 | `play_callback::on_playback_new_track(metadb_handle_ptr)` | 订阅用 `flag_on_playback_*` 组合 |
| 读元数据 | `metadb_handle` → `get_full_info_ref()` → `file_info::meta_find_ex` / `meta_enum_value` | **v2 用 `metadb_v2_rec_t::query_v2`**，以 `FOOBAR2000_TARGET_VERSION >= 81` 条件编译兼容新旧 |
| 读写标签 / LRC | 见 `foo_openlyrics` 的 `src/lyric_io.cpp`、`src/tag_util.cpp` | 可直接参考实现 |
| 状态缓存 | `metadb_index`（`lyric_metadb_index_client`） | 给整个媒体库建歌词索引，支撑"不播放也批量搜歌词" |

> ⚠️ `on_playback_time` 每秒一次、主线程——**逐字歌词的平滑滚动不能靠它驱动**，它只适合做"换到第几行"的锚点；行内插值要用高精度计时器自己在渲染线程算。

---

## 3. DUI vs CUI：一个模板同时出两套面板

| | Default UI (DUI) | Columns UI (CUI) |
|---|---|---|
| 实现 | `ui_element_instance` + `ui_element_impl_withpopup<T>` | 必须实现 `uie::window`：`get_wnd` / `get_type` / `is_available` / `create_or_transfer_window` / `destroy_window`，配合 `window_factory<T>` |
| 配色字体 | 从 `ui_element_instance_callback` 拿 | 通过 `ui_element_instance_callback_v3::query_color` / `query_font_ex` 桥接 |

**强烈建议直接复用 `foo_openlyrics` 的 `uie_shim_panel<T>` 模板**（`src/uie_shim_panel.h`，已核实源码存在）：一个实现同时产出 DUI + CUI 两套面板，省掉两边各写一遍的重复劳动。这是本项目最值得抄的一处结构。

---

## 4. 32 / 64 位

- **foobar2000 1.x 仅 32 位**（官方 /old 页每个版本只有一个安装包）。
- **2.x 每个版本给三个包：32 位 / 64 位 / ARM(arm64ec)**。
- ARM 版通过**仿真**兼容 x64 组件（即 x64 组件能在 ARM 上跑）。
- **双架构做法 = 同一个工程两个 Platform 配置分别编译**，产物按 §5 的约定摆进同一个 `.fb2k-component`。

---

## 5. 打包（最容易踩坑的一节）

**`.fb2k-component` 就是改名的 zip**，且目录结构有硬约定：

```
foo_xxx.fb2k-component
├── foo_xxx.dll          ← 32 位（根目录）
└── x64/foo_xxx.dll      ← 64 位（必须叫 x64/）
```

**其他文件一律不要放。** 已知事故：vgmstream 曾因为在包里多放了一个 README，导致 64 位 DLL 装不到位、**64 位宿主完全不加载该组件**（vgmstream issue #1345）。这是"多一个文件毁掉整个 64 位版本"的典型。

- **PDB 必须单独打一个 zip**，不要塞进主包。
- 安装：双击 `.fb2k-component` 弹安装框，等价入口 `Preferences > Components > Install`。

---

## 6. 许可（法务关键）

- **现行 2.0 SDK 许可为 BSD 式三段条款**：**没有禁止商业销售组件，也没有禁止再分发 SDK** 的条款（已取到原文）。
- ⚠️ 但 **2015 版 SDK 许可里存在一段 "Usage restrictions"**（doxygen 索引可见该字样，正文未取回 = **未验证**）。
- **结论：若要卖组件，务必以随包 `sdk-license.txt` 原文为准**，不要依赖二手转述。
- foobar2000 程序本体的许可只允许**原样再分发安装包**。

---

## 7. 官方仓库收录

**未找到公开的自助提交入口或成文条款（未验证）。**

可观察到的事实：

- 收录**不要求开源**（仓库里如 ASIO Output、Masstagger 等并无源码链接）；
- **未见禁止收费的条款（未验证）**；
- 每个组件页都有 "Discussion" 链接指向 HydrogenAudio 讨论帖。

→ 实际路径 = **GitHub Releases 分发 + HydrogenAudio 论坛讨论帖 + 人工收录**。也就是说 GitHub 是主渠道，官方仓库是加分项，不是必经之路（ESLyric 就不在里面）。

---

## 8. 已知坑清单

| # | 坑 | 后果 |
|---|---|---|
| 1 | `console::print` / `formatter` 线程安全，但**不得在 DLL 静态构造/析构期间调用** | 崩溃 |
| 2 | `ensure_main_thread()` 误用 | 直接 `uBugCheck` |
| 3 | CUI 会把创建时带 `WS_VISIBLE` 的面板记为警告 | 警告刷屏 |
| 4 | DUI 的默认字体/颜色**只能**从 `ui_element_instance_callback` 拿 | 面板不在布局上时拿不到 |
| 5 | `DECLARE_COMPONENT_VERSION` **每个 DLL 只能有一个** | 多个会被当成版本 0 |
| 6 | `VALIDATE_COMPONENT_FILENAME` | 防改名/被重复加载 |
| 7 | 播放进度回调每秒一次且在主线程 | 见 §2 的渲染提醒 |

**未验证项**：安全模式细节、官方 VS 兼容性页内容（Cloudflare 拦截）、静态/动态 CRT 的官方建议（SDK 未明示）。

---

## 附：参考位置

- SDK 内：`component_client.cpp`、`foobar2000/SDK/`、`foobar2000/helpers/`
- foo_openlyrics 关键文件：`src/main.cpp`、`src/uie_shim_panel.h`、`src/ui_lyrics_panel.cpp`、`src/lyric_io.cpp`、`src/tag_util.cpp`、`src/lyric_metadb_index_client.cpp`、`src/sources/`
- vgmstream 打包事故：vgmstream issue #1345
- foobar2000 旧版本页（确认 1.x 仅 32 位）：<https://www.foobar2000.org/old>
- 官方 SDK（许可原文随包）：<https://www.foobar2000.org/SDK>
