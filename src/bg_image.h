#pragma once

#include "bg_math.h"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 面板背景图的**加载与缓存**。
//
// 这一层依赖 COM / WIC，所以进不了离线单测台 —— 纯计算都在 bg_math 那边。
// 划这条线的代价是"解码本身没测试覆盖"，收益是"模糊和适配这两块最容易错的
// 逻辑被测住了"。解码那部分用的是系统组件，出错只会是"读不到文件"，
// 而那种失败是可枚举、可如实上报的。
//
// 【为什么必须有缓存】模糊是 O(w*h) 的整幅扫描，缩放也是 —— 而面板重绘是
// 每秒几十次的事（播放时 4 次/秒、悬停时 50 次/秒）。不在加载时算一次存下来，
// 每帧都要重算几百万次乘加，面板会直接卡死。
// 所以这个模块的对外契约就是：**同一个参数组合只算一次**。
// ---------------------------------------------------------------------------

namespace lyricus {

// 一块处理好的背景图：BGRA、尺寸等于目标区域（面板客户区）。
//
// 处理链：解码 -> 按 BgFit 缩放/裁剪 -> 模糊 -> 压暗+不透明度。
// 出来之后绘制侧只剩"贴上去"这一件事（分层路径直接混进 DIB，
// 非分层路径 StretchDIBits）。
struct BgBitmap {
    int width  = 0;
    int height = 0;
    std::vector<unsigned char> bgra;   // 长度 = width * height * 4

    bool valid() const {
        return width > 0 && height > 0 &&
               bgra.size() == static_cast<size_t>(width) * height * 4;
    }
};

// 取背景图。**同一个参数组合只算一次**，结果留在内部缓存里。
//
// 参数任一变化都会触发重算：路径、目标尺寸（用户拖窗口）、适配方式、
// 模糊半径、压暗、不透明度。
//
// 失败（路径为空、文件不存在、解码失败、尺寸非法）返回 **nullptr** ——
// 调用方按"没有背景图"处理，也就是回到纯色底。
// 失败原因可以用 LastBgError() 取到，写日志用。
const BgBitmap* GetPanelBackground(const std::wstring& path,
                                   int dstW, int dstH,
                                   BgFit fit, int blurPx,
                                   int dimPct, int opacityPct);

// 丢掉缓存。面板销毁、或者用户清了背景图时调用。
void ClearPanelBackgroundCache();

// 上一次失败的原因（宽字符，给 DebugLog 用）。成功时返回空串。
const wchar_t* LastBgError();

} // namespace lyricus
