#include "stdafx.h"
#include "svg_icon.h"
#include "lyric.h"        // 借用 WideToUtf8 / Utf8ToWide
#include "debug_log.h"

#include <lunasvg.h>

#include <algorithm>
#include <map>

namespace lyricus {
namespace {

std::map<std::wstring, RasterIcon>& IconCache() {
    static std::map<std::wstring, RasterIcon> cache;
    return cache;
}

// 读整个文件到内存。
// 不走 lunasvg 的 loadFromFile —— 它内部是 std::ifstream，宽字符路径在 MSVC 上
// 未必能打开；自己读进来再 loadFromData 更稳，也顺便能限制大小。
bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool ok = false;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (4 << 20)) {
        out.assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr) && read > 0) {
            out.resize(read);
            ok = true;
        }
    }
    CloseHandle(h);
    return ok;
}

} // namespace

const RasterIcon* GetSvgIcon(const std::wstring& svgPath, int targetHeight) {
    if (targetHeight <= 0 || svgPath.empty()) return nullptr;

    const std::wstring key = svgPath + L"@" + std::to_wstring(targetHeight);
    auto& cache = IconCache();

    const auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second.Valid() ? &it->second : nullptr;
    }

    RasterIcon icon;
    std::string data;

    if (!ReadFileBytes(svgPath, data)) {
        DebugLog("SVG 读取失败: %s", WideToUtf8(svgPath).c_str());
    } else {
        // lunasvg 只认 SVG 自己的 transform 属性，**不处理根元素上的 CSS transform**
        // （style="transform: ..."）。实测用 CSS scaleX(-1) 做镜像的图标完全没有翻转，
        // 上一首和下一首长得一模一样。这里自己识别并水平翻转。
        const bool mirrorX = (data.find("scaleX(-1)") != std::string::npos)
                          || (data.find("scaleX( -1)") != std::string::npos);

        auto doc = lunasvg::Document::loadFromData(data);
        if (!doc) {
            DebugLog("SVG 解析失败: %s", WideToUtf8(svgPath).c_str());
        } else {
            const double srcW = doc->width();
            const double srcH = doc->height();
            if (srcW > 0.0 && srcH > 0.0) {
                const int h = targetHeight;
                const int w = (std::max)(1, static_cast<int>(h * srcW / srcH + 0.5));

                auto bmp = doc->renderToBitmap(w, h);
                if (bmp.valid()) {
                    icon.width  = bmp.width();
                    icon.height = bmp.height();
                    icon.bgra.assign(static_cast<size_t>(icon.width) * icon.height * 4, 0);

                    const int stride = bmp.stride();
                    const unsigned char* src =
                        reinterpret_cast<const unsigned char*>(bmp.data());

                    // lunasvg 的像素是 uint32，内存里按小端是 B,G,R,A。
                    // 我们只取 A —— 颜色留到混合时按状态给。
                    for (int y = 0; y < icon.height; ++y) {
                        const unsigned char* srow = src + static_cast<size_t>(y) * stride;
                        unsigned char* drow = icon.bgra.data()
                                            + static_cast<size_t>(y) * icon.width * 4;
                        for (int x = 0; x < icon.width; ++x) {
                            drow[x * 4 + 3] = srow[x * 4 + 3];
                        }
                    }

                    if (mirrorX) {
                        // 只有 alpha 通道有意义，所以逐行交换 alpha 字节即可
                        for (int y = 0; y < icon.height; ++y) {
                            unsigned char* row = icon.bgra.data()
                                               + static_cast<size_t>(y) * icon.width * 4;
                            for (int x = 0, x2 = icon.width - 1; x < x2; ++x, --x2) {
                                const unsigned char t = row[x * 4 + 3];
                                row[x * 4 + 3]  = row[x2 * 4 + 3];
                                row[x2 * 4 + 3] = t;
                            }
                        }
                        DebugLog("SVG 检测到 CSS scaleX(-1)，已手动水平翻转");
                    }

                    DebugLog("SVG 光栅化: %s -> %dx%d (源 %.1fx%.1f)",
                             WideToUtf8(svgPath).c_str(), icon.width, icon.height, srcW, srcH);
                }
            } else {
                DebugLog("SVG 尺寸异常: %s (%.1f x %.1f)",
                         WideToUtf8(svgPath).c_str(), srcW, srcH);
            }
        }
    }

    const auto res = cache.emplace(key, std::move(icon));
    return res.first->second.Valid() ? &res.first->second : nullptr;
}

void BlendIcon(unsigned char* dst, int dstW, int dstH, int stride,
               int x, int y, const RasterIcon& icon, unsigned rgb) {
    if (dst == nullptr || !icon.Valid()) return;

    const unsigned tr = (rgb >> 16) & 0xFFu;
    const unsigned tg = (rgb >> 8) & 0xFFu;
    const unsigned tb = rgb & 0xFFu;

    for (int iy = 0; iy < icon.height; ++iy) {
        const int dy = y + iy;
        if (dy < 0 || dy >= dstH) continue;

        const unsigned char* srow = icon.bgra.data() + static_cast<size_t>(iy) * icon.width * 4;
        unsigned char* drow = dst + static_cast<size_t>(dy) * stride;

        for (int ix = 0; ix < icon.width; ++ix) {
            const int dx = x + ix;
            if (dx < 0 || dx >= dstW) continue;

            const unsigned a = srow[ix * 4 + 3];
            if (a == 0) continue;

            // 源：染色后的预乘值
            const unsigned sr = tr * a / 255u;
            const unsigned sg = tg * a / 255u;
            const unsigned sb = tb * a / 255u;
            const unsigned inv = 255u - a;

            unsigned char* d = drow + static_cast<size_t>(dx) * 4;
            // 预乘 source-over：dst = src + dst * (1 - a)
            const unsigned nb = sb + static_cast<unsigned>(d[0]) * inv / 255u;
            const unsigned ng = sg + static_cast<unsigned>(d[1]) * inv / 255u;
            const unsigned nr = sr + static_cast<unsigned>(d[2]) * inv / 255u;
            const unsigned na = a  + static_cast<unsigned>(d[3]) * inv / 255u;

            d[0] = static_cast<unsigned char>(nb > 255u ? 255u : nb);
            d[1] = static_cast<unsigned char>(ng > 255u ? 255u : ng);
            d[2] = static_cast<unsigned char>(nr > 255u ? 255u : nr);
            d[3] = static_cast<unsigned char>(na > 255u ? 255u : na);
        }
    }
}

} // namespace lyricus
