#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SVG 图标：光栅化 + 按 alpha 染色 + 混合
//
// 设计取舍：
//   我们拿到的图标是**单色**的（fill="#cdd6f4"），所以光栅化时**只保留 alpha
//   通道**、丢弃颜色，真正上色放到混合阶段。好处是一套 SVG 就能覆盖
//   普通 / 悬停 / 按下 三种状态色，不用准备三份文件。
//
//   代价：**支持不了多色 SVG**。将来若真需要，得改成保留原始 RGBA。
//
// 混合必须发生在分层渲染的「alpha 修正」之后 —— 那一步会把所有非背景像素
// 的 alpha 强行提到 255，先混进来的半透明图标会被它毁掉边缘。
// ---------------------------------------------------------------------------

namespace lyricus {

struct RasterIcon {
    int width  = 0;
    int height = 0;
    std::vector<unsigned char> bgra;   // BGRA；只有 A 有意义（见上）

    bool Valid() const {
        return width > 0 && height > 0 && bgra.size() == static_cast<size_t>(width) * height * 4;
    }
};

// 按目标高度光栅化（保持宽高比）。结果按 (路径, 高度) 缓存，重复调用不重新解析。
// 失败返回 nullptr。
const RasterIcon* GetSvgIcon(const std::wstring& svgPath, int targetHeight);

// 以 rgb(0xRRGGBB) 染色，按预乘 alpha 混合到 dst。
// dst 必须是预乘 alpha 的 BGRA，stride 以字节计。
void BlendIcon(unsigned char* dst, int dstW, int dstH, int stride,
               int x, int y, const RasterIcon& icon, unsigned rgb);

} // namespace lyricus
