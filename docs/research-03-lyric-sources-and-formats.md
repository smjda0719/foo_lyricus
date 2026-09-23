# 在线歌词源与歌词格式现状

> 来源：子代理联网调研（2026-09）。条目按证据强度分层——「实测」「一手源码」为强证据；标注「未验证」的未取得一手证据。
> 配套：`research-01-market-and-toolchain.md`、`research-02-sdk-component-and-publishing.md`

---

## 1. 主流歌词源可用性

### 1.1 LRCLIB —— 建议首选

- **免 key、免注册**，官方称 "generous rate limiting"；但**要求 User-Agent 标识客户端**（浏览器可用 `X-User-Agent` / `Lrclib-Client`）。超限返回 `429 + Retry-After`，官方要求**必须遵守**，否则可能临时封禁；建议**串行请求 + 200–500ms 间隔**。([API 文档源文件](https://github.com/tranxuanthang/lrclib-homepage/blob/main/src/docs/api.md))
- 端点：
  - `GET /api/get`（`track_name` / `artist_name` / `album_name` / `duration` **全必填**，时长容差 **±2s**）
  - `GET /api/get/{id}`
  - `GET /api/search`（`q` 或 `track_name` 二选一，最多 20 条、**无分页**）
  - `POST /api/request-challenge`（PoW，5 分钟过期）
  - `POST /api/publish`（需 `X-Publish-Token`，匿名可投稿，保留历史版本）
- **实测**（匿名直连，CORS `Access-Control-Allow-Origin: *`）：返回 `plainLyrics` / `syncedLyrics`，另有新字段 **`lyricsfile`**：YAML，行级 `start_ms`/`end_ms` + `words[]` 词级数组（样本中多为空）。
  [实测样本](https://lrclib.net/api/get?artist_name=Coldplay&track_name=Yellow&album_name=Parachutes&duration=269) ｜ [架构文档](https://github.com/tranxuanthang/lrclib/blob/main/ARCHITECTURE.md)
- 匹配策略"模糊但窄"：去标点、小写、时长 ±2s、优先含同步歌词的记录；服务端 24h 缓存，过载返回 `503 + Retry-After`。
- 数据规模：官方未公布总量（**未验证**），实测 `/api/search` 返回的 track id 已达 **3,700 万**量级（弱证据）。
- 版权态度：自述为免费开放的同步歌词查找/投稿服务，有 `/api/flag` 举报接口；侵权处理条款**未验证**。
- **结论：首选源。**

### 1.2 网易云音乐

- `GET https://music.163.com/api/song/lyric?id=&lv=&kv=&tv=`
  **实测（2026-09-22）匿名可用**，返回 `lrc` / `klyric` / `tlyric`（翻译）/ `romalrc`（罗马音）。
- `/api/song/lyric/v1` 的 `lrc.lyric` 已是逐行富文本 JSON：`{"t":0,"c":[{"tx":"…"}]}`。
- **实测未返回 `yrc`** —— 逐字很可能需要 eapi/weapi 加密参数或登录态（**未验证**）。
- 风控（[第三方 API 文档](https://raw.githubusercontent.com/sssnan/NeteaseCloudMusicApi/684c581fe002e46a0164d5a0b1e5cc2da7a118f2/docs/README.md)）：不缓存会触发"IP 高频"、国外/部分云报 `460 cheating 异常`、未登录调受保护接口返回 301。
- YRC 结构：`[lineStart,lineDur](sylStart,sylDur,0)text`，第三位含义未知。([Lyricify 格式文档](https://docs.lyricify.app/en/lyrics/guide/))

### 1.3 QQ 音乐（QRC）与酷狗（KRC）

- 时间戳位置**相反**：
  - **QRC**：`[lineStart,lineDur]text(sylStart,sylDur)` —— 词时间戳在文本**后**
  - **YRC**：`[lineStart,lineDur](sylStart,sylDur)text` —— 词时间戳在文本**前**
  ([AMLL](https://amll.dev/en/guides/lyric/formats))
- **QRC 解密链**：hex → **3DES**（ASCII 密钥 `!@#)(*$%123ZXC!@!@#)(NHL`）→ zlib → 去 UTF-8 BOM → UTF-8（内容为 XML）。
  ([Qrc/Decrypter.cs](https://github.com/WXRIW/Lyricify-Lyrics-Helper/blob/master/Lyricify.Lyrics.Helper/Decrypter/Qrc/Decrypter.cs) ｜ [tripledes.py](https://github.com/L-1124/QQMusicApi/blob/2290a323/qqmusic_api/algorithms/tripledes.py))
- **KRC 解密链**：Base64 → **丢前 4 字节** → 与 16 字节密钥
  `40 47 61 77 5E 32 74 47 51 36 31 2D CE D2 6E 69` **逐字节 XOR** → zlib → UTF-8。
  ([Krc/Decrypter.cs](https://github.com/WXRIW/Lyricify-Lyrics-Helper/blob/master/Lyricify.Lyrics.Helper/Decrypter/Krc/Decrypter.cs))
- 两者在 Lyricify 里标注"无需配置凭据"，但都是**私有格式、无官方文档**，接口/密钥随时可能失效。

### 1.4 商业 / 平台源

- **Musixmatch**：必须 `apikey`；自助条款写明 **FOR NOT COMMERCIAL USE**，商用需联系 sales；响应带 `restricted` 标记时（按艺人/单曲/国家）**不返回歌词正文**。
  ([入门](https://docs.musixmatch.com/lyrics-api/introduction) ｜ [内容限制](https://docs.musixmatch.com/content-restrictions) ｜ [Terms](https://about.musixmatch.com/apiterms))
- **Spotify**：**无公开歌词 API**；Lyricify 需自备 `sp_dc` Cookie；社区方案自述 powered by Musixmatch 且 "probably against Spotify TOS"。
  ([Lyricify](https://github.com/WXRIW/Lyricify-Lyrics-Helper) ｜ [spotify-lyrics-api](https://github.com/akashrchandran/spotify-lyrics-api))
- **Apple Music**：TTML 歌词，必须 `Media User Token`。
- **Genius**：官方 API 仅 `/search`，**不返回歌词正文**。([OpenAPI](https://raw.githubusercontent.com/api-evangelist/genius/refs/heads/main/openapi/genius-search-api-openapi.yml))
- **Deezer / MusicBrainz**：无权威说明（**未验证**）。

### 1.5 可复用的聚合实现

| 项目 | 许可 | 价值 |
|---|---|---|
| [Lyricify-Lyrics-Helper](https://github.com/WXRIW/Lyricify-Lyrics-Helper) | Apache-2.0（.NET） | **最值得借鉴设计**：8 源 + LRC/QRC/KRC/YRC/TTML 解析与生成 + 解密 + 统一行/词模型 |
| [LDDC](https://github.com/chenmozhijin/LDDC) | GPLv3（Python） | QQ/酷狗/网易/LRCLIB；导出逐字 LRC、逐行 LRC、增强型 LRC、SRT、ASS；写标签或文件；配套 foobar2000 插件 `foo_lddc` |
| [LRCGET](https://github.com/tranxuanthang/lrcget) | — | LRCLIB 官方客户端：扫描曲库 → 下载同名 `.lrc` 到音频目录 |

---

## 2. 歌词格式与时间轴

| 格式 | 行/词 | 关键结构 | 备注 |
|---|---|---|---|
| LRC | 行 | `[mm:ss.xx]text`；一行可多时间戳、**不允许重复时间戳**；`[ti:/ar:/al:/by:/offset:]` | 无正式标准，变体极多 |
| 增强 LRC（A2） | 词 | `[00:00.00] <00:00.04> When <00:00.16> the …`，尖括号标出**词起点** | `.lrc` / `.alrc` |
| **ESLyric 逐字** | 词 | **省掉行时间戳**，首个词的时间戳当行起点，后续词只写结束时间 | **foobar2000 生态最相关** |
| SPL（Salt Player） | 词 | LRC 家族里罕见的正式规范；同时间戳下行 = 译文 | 可当解析基准 |
| YRC / QRC / KRC | 词 | `[行起点,行时长]` + 词级 `(起点,时长)`，YRC 在前、QRC 在后 | 前两者私有；KRC 需 XOR 解密 |
| TTML | 行+词 | W3C XML；`itunes:timing="Word\|Line"`、`ttm:role="x-translation\|x-roman\|x-bg"`、`ttm:agent`、ruby、`<iTunesMetadata>` 旁挂翻译 | **最适合做统一中间格式** |

**双语歌词的四种表达**：

1. 普通 LRC **无原生方案** —— 同时间戳另起一行（译文紧随原文），解析器不特殊处理，由业务层归类；
2. SPL **显式规定**同时间戳 = 译文；
3. TTML 用 `x-translation` / `x-roman` 或 head sidecar；
4. Lyricify Quick Export 用 `[translation: format@LRC]` 分块。

([AMLL](https://amll.dev/en/guides/lyric/formats) ｜ [TTML 详情](https://amll.dev/en/guides/lyric/ttml))

**编码问题（有实证）**：常见 UTF-8 无/带 BOM、UTF-16、GBK/GB18030 混用。
实战 bug：foo_openlyrics 读 UTF-16 歌词时命中 `\0` 提前截断，导致**整篇乱码**，1.5 才修（[issue #232](https://github.com/jacquesh/foo_openlyrics/issues/232)）；QRC 解密流程专门要"去除 UTF-8 BOM"。

---

## 3. 工程建议

**数据模型**

```
Song  { title, artist, album, durationMs, source, sourceId }
Lyric { lines[], syncLevel: none|line|word|mixed, raw }
Line  { startMs, endMs, text, translation?, romanization?,
        isBackground?, agent?, words[] }
Word  { startMs, endMs, text }
```

保留 `raw` 与来源，便于降级重建 LRC。

**缓存优先级**

1. 音频**同目录同名 `.lrc`**（foobar2000 事实约定）
2. 媒体库标签（foo_openlyrics 支持本地文件 / ID3 标签 / 网络；LDDC 可写标签）
3. 本地 SQLite `(track_key, source, fetched_at, raw, sync_level)`

`track_key` 用规范化 `title|artist|album|duration_ms`，按 **±2s** 判同曲；**404 也要负缓存**。

**网络层**：串行 + 200–500ms 间隔、尊重 `Retry-After`（LRCLIB 官方硬要求）。

**解析**：生成 A2 / ESLyric 逐字时**原样保留空白**（AMLL 明确不合并、不裁剪），否则逐字错位。

**合规**：Musixmatch 明确非商用；Spotify 社区方案自认违反 ToS；QQ/酷狗/网易为私有接口 + 加密格式，逆向解密有法律与稳定性双重风险（网易另有 IP 高频 / 460 风控）。
→ 建议**默认仅启用 LRCLIB + 本地文件**，第三方平台做显式可选、默认关闭；不内置商业平台密钥/Cookie，不随插件分发歌词数据，附免责声明。

> 注：本项目为**自用**（见 `decisions.md` D-003），合规压力大幅降低，但上述"默认关闭第三方源"的设计仍然值得保留——因为私有接口和密钥**随时会失效**，这是稳定性问题，不只是法律问题。

**结论**：LRCLIB 作首选在线源；本地 `.lrc`（含增强 LRC）作一级缓存；内部统一模型保留**行/词两级时间轴**；TTML 作可选的高保真导入/导出格式。
