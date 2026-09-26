# 环境准备

`3rdparty/` 和 `downloads/` 不进版本库（277 MB，而且各依赖有各自的许可）。
新 clone 下来的仓库是编不了的，先跑一次准备脚本。

## 一条命令

```powershell
.\tools\setup.ps1
```

它会检查缺什么、缺什么下什么。已经有的会跳过，所以重跑是安全的。

```
.\setup.ps1 -Force        # 全部重新下载
.\setup.ps1 -SkipSdk      # 只补某一个（-SkipSdk / -SkipWtl / -SkipLunasvg / -SkipCui）
```

## 四个依赖

| 依赖 | 用途 | 来源 |
|---|---|---|
| **foobar2000 SDK** | 组件的头文件与导入库 | foobar2000 官网的 `.7z` |
| **WTL 10** | SDK 不自带 `atlapp.h`，libPPUI 编译需要 | GitHub（按单个文件取） |
| **lunasvg** | 把 SVG 图标光栅化成位图（纯 CPU，MIT） | GitHub（按单个文件取） |
| **columns_ui-sdk** | CUI 面板接口 | GitHub（**锁定了提交版本**） |

`columns_ui-sdk` 的版本在 `setup.ps1` 顶部一个常量里锁着。要升级就改那里，
然后重新编译验证 —— 不要跟着 master 走。

## 注意代理默认值

脚本的 `-Proxy` 参数**默认是 `http://127.0.0.1:8902`**。
如果你本机没有这个端口的代理，所有下载都会失败。这时显式关掉：

```powershell
.\tools\setup.ps1 -Proxy ''
```

## 脚本的下载策略

对不稳定网络做过适配，值得知道：

- GitHub 的 `/archive/refs/heads/*.zip` 是**服务端即时生成**的，
  不支持 HTTP Range，断点续传无效。所以 WTL 和 lunasvg 改成
  **走 GitHub tree API 按单个文件下载** —— 单文件小，失败重试代价低，
  而且 `raw.githubusercontent.com` 支持 Range。
- 所有下载都经过 `tools/fetch.ps1`（带续传、退避重试、大小校验）。

## 目录结构

`3rdparty/` 下：

```
foobar2000/
  SDK/            头文件（foobar2000.h、ui_element.h、...）
  helpers/        ATL 辅助模板
  shared/         shared.dll 相关
  pfc/            pfc（含 libPPUI 需要的部分）
libPPUI/
WTL/Include/
lunasvg/
columns_ui-sdk/
```

具体路径以 `build/foo_lyricus.vcxproj` 里的 `AdditionalIncludeDirectories`
为准 —— 那是唯一的事实来源，目录名对不上就改那里。

## 验证

```powershell
.\build.ps1
```

能出 `bin\foo_lyricus.dll` 就成了。**0 warning 是预期状态** —— 出现 warning
通常说明 SDK 版本和代码预期的不一致，值得看一眼。

## 不要提交 `3rdparty/`

`.gitignore` 已经排除了它，但如果你改了目录名、让它落到规则之外，
`git status` 会显示出来。那些文件有各自的许可，混进这个库会说不清。

## 32 位

**不支持**。`foobar2000_SDK.lib`、`component_client.lib`、`pfc.lib`
目前都只有 x64 版，要补得先自己构建它们的 32 位版本。
