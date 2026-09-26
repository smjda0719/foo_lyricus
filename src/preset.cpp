#include "preset.h"
#include "lyric.h"        // WideToUtf8 / Utf8ToWide（纯转换，不碰 SDK）

#include <algorithm>
#include <cctype>     // isxdigit
#include <cstdio>     // sprintf_s
#include <cstdlib>    // strtoul / strtol
#include <utility>    // std::move

// ---------------------------------------------------------------------------
// 外观预设的纯逻辑。见 preset.h 里"为什么只装外观"。
//
// 这一层刻意只依赖 windows.h + lyric.h，不碰 config / SDK ——
// 于是解析、格式化、增删改这些真正容易出错的部分能在离线单测台里反复扫。
// ---------------------------------------------------------------------------

namespace lyricus {
namespace {

// ---- 颜色 <-> "RRGGBB" ----

std::string ColorToHex(COLORREF c) {
    char buf[8];
    sprintf_s(buf, "%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

// 解析 "RRGGBB" / "rrggbb" / 带前导 '#'。失败返回 false。
bool HexToColor(const std::string& s, COLORREF& out) {
    std::string v = s;
    if (!v.empty() && v[0] == '#') v.erase(0, 1);
    if (v.size() != 6) return false;

    for (char ch : v) {
        if (!isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    const unsigned long n = strtoul(v.c_str(), nullptr, 16);
    out = RGB((n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF);
    return true;
}

// ---- 数值 ----

int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool ParseInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;   // 有尾巴 = 不是纯数字
    out = static_cast<int>(v);
    return true;
}

// 名字 / 字体族里出现 TAB 或换行会把整张表撑坏 —— 存进去之前换掉。
// 用空格而不是直接删：字体名里的空格有意义（"Microsoft YaHei UI"）。
void Sanitize(std::wstring& s) {
    for (wchar_t& c : s) {
        if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    }
}

// 夹取到合法区间的那些字段。解析之后统一走一遍，
// 这样"手改配置文件写了个 999"和"别人给的预设"都进不来坏值。
AppearancePreset ClampPreset(AppearancePreset p) {
    // ⚠️ 这里用 preset.h 里的常量，不写字面量 —— 它们和单测的断言共用同一份，
    //    免得改了实现忘了改断言（backdropMode 刚这么挂过一次）。
    //
    // 关于 alpha：这里放到 0..255，而 config.cpp 的 ClampAlpha 会再夹到
    // 60..kMaxAlpha。**分层是有意的** —— 预设定的是"这个颜色方案的不透明度"，
    // 而"能不能真的设成 5"是配置层的规则。预设不该替它做决定，
    // 否则从别处导入一套 alpha=30 的配色会被静默改掉再存回去。
    p.alpha        = ClampInt(p.alpha, kPresetMinAlpha, kPresetMaxAlpha);
    p.fontPct      = ClampInt(p.fontPct, kPresetMinFontPct, kPresetMaxFontPct);
    p.backdropMode = ClampInt(p.backdropMode, kPresetMinBackdrop, kPresetMaxBackdrop);
    // 手改配置写了个 7 之类的值进来，退回自动（安全的那一档）
    p.ctrlMode     = ClampInt(p.ctrlMode, kCtrlMin, kCtrlMax);

    // 背景图参数。模糊和压暗都必须夹：写个 999 进来的话，
    // 模糊会实打实地跑很久（迭代次数乘上整幅像素），压暗过头会让图全黑。
    p.bgFit     = ClampInt(p.bgFit,     kBgFitMin,     kBgFitMax);
    p.bgBlur    = ClampInt(p.bgBlur,    kBgBlurMin,    kBgBlurMax);
    p.bgDim     = ClampInt(p.bgDim,     kBgDimMin,     kBgDimMax);
    p.bgOpacity = ClampInt(p.bgOpacity, kBgOpacityMin, kBgOpacityMax);

    // 手动构图（D-103）。走 ClampBgManual 而不是逐个夹 ——
    // 三个值是一组语义（"图必须盖住区域"），分开夹容易漏掉某一个，
    // 而漏掉的那个会让画面露出一条底色边。
    {
        BgManual m;
        m.zoomPct    = p.bgZoomPct;
        m.offsetXPct = p.bgOffsetXPct;
        m.offsetYPct = p.bgOffsetYPct;
        const BgManual c = ClampBgManual(m);
        p.bgZoomPct    = c.zoomPct;
        p.bgOffsetXPct = c.offsetXPct;
        p.bgOffsetYPct = c.offsetYPct;
    }
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// 内置
// ---------------------------------------------------------------------------

std::vector<AppearancePreset> BuiltinPresets() {
    std::vector<AppearancePreset> v;

    // ---- ① 默认 ----
    // 就是出厂的那一套：深色底 + 白字 + 跟随宿主字体。
    // 放在第一个，因为它是"什么都没改"的样子。
    {
        AppearancePreset p;
        p.name    = L"默认";
        p.header  = RGB(235, 235, 240);
        p.current = RGB(255, 255, 255);
        p.normal  = RGB(172, 172, 180);
        p.dim     = RGB(150, 150, 158);
        p.warn    = RGB(205, 165, 165);
        p.bg      = RGB(28, 28, 30);
        p.alpha   = 215;
        p.fontFace = "";        // 空 = 跟随宿主界面字体
        p.fontPct  = 100;
        p.backdropMode = 4;     // Translucent（出厂默认）
        v.push_back(p);
    }

    // ---- ② 暗色 ----
    // 比默认更沉：底色近纯黑、当前行改用淡蓝（黑底上比纯白更"跳"但不刺眼）。
    // alpha 也高一些 —— 选这个的人想要的是"看得清"，不是"透出去"。
    {
        AppearancePreset p;
        p.name    = L"暗色";
        p.header  = RGB(200, 200, 205);
        p.current = RGB(120, 190, 255);
        p.normal  = RGB(150, 150, 155);
        p.dim     = RGB(110, 110, 115);
        p.warn    = RGB(230, 150, 150);
        p.bg      = RGB(12, 12, 14);
        p.alpha   = 235;
        p.fontFace = "";
        p.fontPct  = 100;
        p.backdropMode = 4;     // Translucent
        v.push_back(p);
    }

    // ---- ③ 亮色 ----
    // 浅底深字。当前行用蓝 —— 浅色底上蓝色比黑色更显眼，而且和"当前行"
    // 这个语义天然搭（链接色）。
    {
        AppearancePreset p;
        p.name    = L"亮色";
        p.header  = RGB(90, 90, 100);
        p.current = RGB(0, 90, 180);
        p.normal  = RGB(60, 60, 70);
        p.dim     = RGB(120, 120, 130);
        p.warn    = RGB(180, 60, 60);
        p.bg      = RGB(250, 250, 250);
        p.alpha   = 230;
        p.fontFace = "";
        p.fontPct  = 100;
        p.backdropMode = 4;     // Translucent
        v.push_back(p);
    }

    // ---- ④ 高对比 ----
    // 给视力不好或在强光下看的人。三条都和别的预设不同，而且都有理由：
    //   * 当前行用**亮黄**——纯黑底上它比白色更醒目，且不会和正文的白混；
    //   * alpha **255 完全不透明**、backdrop 也**不透明** ——
    //     半透明会透出后面的内容，正好把对比度毁掉，那就失去这一套的意义了；
    //   * 字号 **115%** —— 选它的人本来就是要"更容易看清"。
    {
        AppearancePreset p;
        p.name    = L"高对比";
        p.header  = RGB(255, 255, 255);
        p.current = RGB(255, 235, 0);
        p.normal  = RGB(215, 215, 215);
        p.dim     = RGB(170, 170, 170);
        p.warn    = RGB(255, 120, 120);
        p.bg      = RGB(0, 0, 0);
        p.alpha   = 255;
        p.fontFace = "";
        p.fontPct  = 115;
        p.backdropMode = 0;     // None：自绘不透明
        v.push_back(p);
    }

    return v;
}

// ---------------------------------------------------------------------------
// 文本表
// ---------------------------------------------------------------------------

std::string FormatPresets(const std::vector<AppearancePreset>& presets) {
    std::string s;
    // 满了就丢最旧的（从头丢）—— 和歌词线索表同一套策略
    const size_t begin = (presets.size() > kMaxPresets) ? presets.size() - kMaxPresets : 0;

    for (size_t i = begin; i < presets.size(); ++i) {
        const AppearancePreset& p = presets[i];
        if (p.name.empty()) continue;

        std::wstring name = p.name;
        Sanitize(name);

        s += WideToUtf8(name);
        s += '\t';

        s += "header=";   s += ColorToHex(p.header);
        s += ";current="; s += ColorToHex(p.current);
        s += ";normal=";  s += ColorToHex(p.normal);
        s += ";dim=";     s += ColorToHex(p.dim);
        s += ";warn=";    s += ColorToHex(p.warn);
        s += ";bg=";      s += ColorToHex(p.bg);
        s += ";alpha=";   s += std::to_string(p.alpha);
        s += ";fontPct="; s += std::to_string(p.fontPct);
        s += ";backdrop="; s += std::to_string(p.backdropMode);

        // 字体族放最后而且**允许为空**（空 = 跟随宿主）。
        // 空字段写在中间会让 key=value 的分隔看起来像少了什么，
        // 放最后则一眼能看出"这条没设字体"。
        if (!p.fontFace.empty()) {
            std::wstring face = Utf8ToWide(p.fontFace.c_str());
            Sanitize(face);
            // 分号会截断字段，换掉（字体名里本来也不该有分号）
            std::string f = WideToUtf8(face);
            for (char& c : f) { if (c == ';') c = ' '; }
            s += ";font="; s += f;
        }

        // 控件配色**只在自定义模式下写出去**。
        //
        // 自动模式下那 4 个基色根本不参与绘制（颜色是从面板底色算的），
        // 把它们写进文件只会让预设变长、还会让人以为它们生效了。
        // 反过来读的时候缺省就是自动，所以老预设天然兼容。
        if (p.ctrlMode == kCtrlCustom) {
            s += ";ctrlMode=1";
            s += ";ctrlBtn=";    s += ColorToHex(p.ctrlButton);
            s += ";ctrlIcon=";   s += ColorToHex(p.ctrlIcon);
            s += ";ctrlSlider="; s += ColorToHex(p.ctrlSlider);
            s += ";ctrlText=";   s += ColorToHex(p.ctrlText);
        }

        // 背景图同样**只在真的设了图时才写**。没有图是常态，
        // 每次都写五个 `bg*=0` 只是让文件变长；反过来读的时候
        // 缺省就是"没有背景图"，于是老预设一个字都不用改。
        if (!p.bgImage.empty()) {
            // 路径里可能有分号（虽然罕见），换掉 —— 分号会截断字段
            std::string img = p.bgImage;
            for (char& c : img) {
                if (c == ';' || c == '\n' || c == '\r') c = ' ';
            }
            s += ";bgImage=";   s += img;
            s += ";bgFit=";     s += std::to_string(p.bgFit);
            s += ";bgBlur=";    s += std::to_string(p.bgBlur);
            s += ";bgDim=";     s += std::to_string(p.bgDim);
            s += ";bgOpacity="; s += std::to_string(p.bgOpacity);
            // 手动构图（D-103）。和别的字段一样：只在设了图时才写。
            // 三个值是一组，一起写出去 —— 只写其中一个的话读回来另外两个
            // 会退回默认（居中、100%），构图就和存的时候不是一回事了。
            if (p.bgFit == static_cast<int>(BgFit::Manual)) {
                s += ";bgZoom=";  s += std::to_string(p.bgZoomPct);
                s += ";bgOffX=";  s += std::to_string(p.bgOffsetXPct);
                s += ";bgOffY=";  s += std::to_string(p.bgOffsetYPct);
            }
        }

        s += '\n';
    }
    return s;
}

std::vector<AppearancePreset> ParsePresets(const std::string& text) {
    std::vector<AppearancePreset> out;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;

        // 容忍 CRLF：编辑器改过的文件会带 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;      // 没有 TAB = 不是一条预设
        const std::string nameUtf8 = line.substr(0, tab);
        if (nameUtf8.empty()) continue;              // 空名字没法引用，跳过

        AppearancePreset p;                          // 先全取默认值
        p.name = Utf8ToWide(nameUtf8.c_str());

        // 逐段解析 key=value。
        //
        // ★ 认不出来的 key **直接忽略**，缺的 key **保持默认值** ——
        //   这两条就是"向前兼容"的全部：以后加字段时，老预设文件照样能读进来，
        //   只是新字段用默认值。反过来（遇到未知 key 就丢整条）会让
        //   "用新版存的预设拿到旧版用"直接失效。
        const std::string rest = line.substr(tab + 1);
        size_t f = 0;
        while (f <= rest.size()) {
            size_t semi = rest.find(';', f);
            if (semi == std::string::npos) semi = rest.size();
            const std::string kv = rest.substr(f, semi - f);
            f = semi + 1;

            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const std::string v = kv.substr(eq + 1);

            if      (k == "header")  HexToColor(v, p.header);
            else if (k == "current") HexToColor(v, p.current);
            else if (k == "normal")  HexToColor(v, p.normal);
            else if (k == "dim")     HexToColor(v, p.dim);
            else if (k == "warn")    HexToColor(v, p.warn);
            else if (k == "bg")      HexToColor(v, p.bg);
            else if (k == "alpha")   ParseInt(v, p.alpha);
            else if (k == "fontPct") ParseInt(v, p.fontPct);
            else if (k == "backdrop") ParseInt(v, p.backdropMode);
            else if (k == "font")    p.fontFace = v;
            // 控件配色。缺省就是自动（kCtrlAuto），所以老预设读进来天然兼容 ——
            // 这正是"只在自定义时才写出去"那个决定的另一半。
            else if (k == "ctrlMode")   ParseInt(v, p.ctrlMode);
            else if (k == "ctrlBtn")    HexToColor(v, p.ctrlButton);
            else if (k == "ctrlIcon")   HexToColor(v, p.ctrlIcon);
            else if (k == "ctrlSlider") HexToColor(v, p.ctrlSlider);
            else if (k == "ctrlText")   HexToColor(v, p.ctrlText);
            // 背景图。缺省就是"没有背景图"，所以老预设天然兼容 ——
            // 和"只在设了图时才写出去"是同一件事的两半。
            else if (k == "bgImage")   p.bgImage   = v;
            else if (k == "bgFit")     ParseInt(v, p.bgFit);
            else if (k == "bgBlur")    ParseInt(v, p.bgBlur);
            else if (k == "bgDim")     ParseInt(v, p.bgDim);
            else if (k == "bgOpacity") ParseInt(v, p.bgOpacity);
            // 手动构图（D-103）。缺省 = 铺满且居中，所以老预设天然兼容。
            else if (k == "bgZoom")    ParseInt(v, p.bgZoomPct);
            else if (k == "bgOffX")    ParseInt(v, p.bgOffsetXPct);
            else if (k == "bgOffY")    ParseInt(v, p.bgOffsetYPct);
            // 其它 key 忽略（见上面的说明）
        }

        out.push_back(ClampPreset(std::move(p)));
    }
    return out;
}

const AppearancePreset* FindPreset(const std::vector<AppearancePreset>& presets,
                                   const std::wstring& name) {
    for (const auto& p : presets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 增删改
// ---------------------------------------------------------------------------

std::string ApplyPresetEdit(const std::string& currentText,
                            const std::wstring& name,
                            const AppearancePreset* preset,
                            bool* changedOut) {
    if (changedOut != nullptr) *changedOut = false;
    if (name.empty()) return currentText;    // 空名字：不动，也不算改动

    auto entries = ParsePresets(currentText);

    bool changed = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name != name) continue;

        if (preset == nullptr) {
            // 删除。用**下标** erase，不用 remove(begin,end,value) ——
            // 后者会把 value（一个引用）在搬移过程中覆盖掉，
            // 属于"读了正在被移动的对象"。歌词线索表那边踩过这个（D-078）。
            entries.erase(entries.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
        } else {
            AppearancePreset p = *preset;
            p.name = name;                   // 以 name 为准，不信任调用方填的
            p = ClampPreset(std::move(p));
            entries[i] = std::move(p);
            changed = true;
        }
        break;
    }

    // 没找到 + 要写 -> 追加
    if (!changed && preset != nullptr) {
        AppearancePreset p = *preset;
        p.name = name;
        p = ClampPreset(std::move(p));
        entries.push_back(std::move(p));
        changed = true;
    }

    if (changedOut != nullptr) *changedOut = changed;
    return changed ? FormatPresets(entries) : currentText;
}

// ---------------------------------------------------------------------------
// 导入 / 导出
// ---------------------------------------------------------------------------

std::string ExportPreset(const AppearancePreset& preset) {
    return FormatPresets({ preset });
}

bool ImportPreset(const std::string& text, AppearancePreset& out) {
    std::string s;

    // 去掉 UTF-8 BOM —— 别人用记事本另存为时会出现
    size_t start = 0;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        start = 3;
    }
    s = text.substr(start);

    // 去掉所有 \r（CRLF / CR）
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());

    // 取第一行非空的内容 —— 用户从聊天窗口粘贴时前后常带空行
    size_t p = 0;
    std::string line;
    while (p < s.size()) {
        size_t eol = s.find('\n', p);
        if (eol == std::string::npos) eol = s.size();
        line = s.substr(p, eol - p);
        p = eol + 1;
        // 去掉前后空白
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;         // 全空白，换下一行
        const size_t e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        break;
    }
    if (line.empty()) return false;

    auto parsed = ParsePresets(line + "\n");
    if (parsed.empty()) return false;

    out = parsed[0];
    return true;
}

} // namespace lyricus
