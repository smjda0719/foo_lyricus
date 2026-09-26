#include "bg_image.h"

#include "debug_log.h"

#include <windows.h>
#include <wincodec.h>

#include <atlbase.h>   // CComPtr

#include <algorithm>
#include <cstring>

namespace lyricus {

namespace {

// 上一次请求的参数 + 结果。
//
// ★ **两个槽**：面板一个、首选项预览一个（D-103）。
//
// 【为什么不是一条，也不是通用 LRU】只有这两个使用者，而且各自的尺寸都稳定
//（面板会被拖动，但那是同一个槽内部参数变化的事）。
//
// ⚠️ 一条缓存是不够的：预览区的尺寸和面板不同，两者交替调用的话会互相
//    覆盖对方的槽、**每次都重算**（而重算一次是几百万次乘加 + 一趟模糊）。
//    表现为"一边好好的、另一边每帧卡"。
//
// 而做成通用 LRU 只是徒增复杂度：内存里多存几份几 MB 的位图，
// 换不来任何一次命中。
struct CacheEntry {
    bool         valid = false;
    std::wstring path;
    int          dstW = 0, dstH = 0;
    BgFit        fit = BgFit::Cover;
    BgManual     manual;                       // 手动构图（D-103）
    int          blur = 0, dim = 0, opacity = 100;
    BgBitmap     bmp;
    unsigned long long used = 0;               // 最近一次命中的序号（挑淘汰对象用）
};

CacheEntry   g_slots[2];
unsigned long long g_tick = 0;
std::wstring g_lastError;

// COM 每线程初始化一次。
//
// ⚠️ 用 COINIT_APARTMENTTHREADED 而不是 MULTITHREADED：
//    foobar2000 主线程是 STA。在已经初始化为 STA 的线程上请求 MTA 会返回
//    RPC_E_CHANGED_MODE 并且**失败**；反过来（已 STA 再请求 STA）只返回
//    S_FALSE，那只是"已经初始化过"，完全可以继续用。
//    所以 STA 是这里唯一安全的选择。
bool EnsureCom() {
    static bool s_tried = false;
    static bool s_ok    = false;
    if (s_tried) return s_ok;
    s_tried = true;

    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_OK / S_FALSE 都算可用。RPC_E_CHANGED_MODE 说明这个线程已经是 MTA ——
    // WIC 不要求 STA，所以也放行。
    s_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    return s_ok;
}

bool SameParams(const CacheEntry& e, const std::wstring& path, int dstW, int dstH,
                BgFit fit, const BgManual& manual,
                int blur, int dim, int opacity) {
    // ⚠️ manual 的三个值必须一起比 —— 它们是「构图」这一件事的三个分量，
    //    漏比任何一个都会让「拖了没反应」，而拖动的正是被漏掉的那个。
    return e.valid && e.path == path &&
           e.dstW == dstW && e.dstH == dstH &&
           e.fit == fit &&
           e.manual.zoomPct    == manual.zoomPct &&
           e.manual.offsetXPct == manual.offsetXPct &&
           e.manual.offsetYPct == manual.offsetYPct &&
           e.blur == blur && e.dim == dim && e.opacity == opacity;
}

// 把一块已缩放的 BGRA 数据贴进目标缓冲。
// offsetX/Y 是左上角；Tile 模式下会反复贴满。
void BlitInto(std::vector<unsigned char>& dst, int dstW, int dstH,
              const unsigned char* src, int srcW, int srcH,
              int offsetX, int offsetY, bool tile) {
    if (src == nullptr || srcW <= 0 || srcH <= 0) return;

    if (!tile) {
        // 逐行拷贝，顺带处理越界：Contain 时图完全在界内，但 Cover 经过
        // 浮点舍入后可能比目标宽/高 1 像素，那时只能裁掉。
        //
        // ⚠️ 这里**手写比较**而不是 std::max/min —— windows.h 把它们定义成了
        //    宏，`std::max(a,b)` 会被预处理器拆成 `std::(((a)>(b))?(a):(b))`，
        //    报 C2589（bg_math.cpp 里踩过一次，项目里也早有记录）。
        //    这两行逻辑本来就简单，写开反而更清楚。
        //
        // ★ 源侧和目的侧**都要查**（D-105）。
        //   曾经只查了目的侧，以为"copyW 是按 dstW 截的、所以不会超源" ——
        //   那只在 offsetX <= 0 时成立：offsetX > 0 时 srcX0 恒为 0、
        //   copyW 初值就是 srcW，看似没问题；但 offsetX 是**手动构图算出来的**，
        //   它可以是任何值（极端缩放下到几万），一旦 dstX0 + srcX0 这类
        //   中间量和 int 边界擦肩，判断就会失效。
        //   与其论证"不会发生"，不如每次乘法之前都确认一遍。
        for (int y = 0; y < srcH; ++y) {
            const int dy = offsetY + y;
            if (dy < 0 || dy >= dstH) continue;

            const int dstX0 = (offsetX > 0) ? offsetX : 0;
            const int srcX0 = (offsetX > 0) ? 0 : -offsetX;

            // 源侧：起点必须在图内
            if (srcX0 < 0 || srcX0 >= srcW) continue;

            int copyW = srcW - srcX0;                       // <= srcW，源侧安全
            if (dstX0 < 0 || dstX0 >= dstW) continue;
            if (copyW > dstW - dstX0) copyW = dstW - dstX0; // 再按目的侧收
            if (copyW <= 0) continue;

            // 双保险：算完再核一次（上面任何一步溢出的话这里会拦住）
            if (srcX0 + copyW > srcW || dstX0 + copyW > dstW) continue;

            std::memcpy(dst.data() + (static_cast<size_t>(dy) * dstW + dstX0) * 4,
                        src + (static_cast<size_t>(y) * srcW + srcX0) * 4,
                        static_cast<size_t>(copyW) * 4);
        }
        return;
    }

    // 平铺：从左上角开始按原尺寸反复贴，超出目标区域的部分由逐行拷贝裁掉
    for (int y = offsetY; y < dstH; y += srcH) {
        for (int x = offsetX; x < dstW; x += srcW) {
            BlitInto(dst, dstW, dstH, src, srcW, srcH, x, y, false);
        }
    }
}

// 从文件名解码出原图尺寸（不解像素），用来算 place。
bool ReadImageSize(IWICImagingFactory* factory, const std::wstring& path,
                   int& outW, int& outH) {
    CComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return false;

    CComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    outW = static_cast<int>(w);
    outH = static_cast<int>(h);
    return true;
}

} // namespace

const BgBitmap* GetPanelBackground(const std::wstring& path,
                                   int dstW, int dstH,
                                   BgFit fit, const BgManual& manual,
                                   int blurPx, int dimPct, int opacityPct) {
    g_lastError.clear();

    if (path.empty())          return nullptr;   // 用户没设背景图 —— 不是错误
    if (dstW <= 0 || dstH <= 0) return nullptr;   // 面板还没尺寸

    // ⚠️ **先查两个槽** —— 命中就直接返回、不动任何状态。
    //
    // 这一步不是优化而是正确性：面板每帧都在调（播放时 4 次/秒、悬停 50 次/秒），
    // 而首选项预览偶尔才调一次。不先查的话两者会交替覆盖对方的槽，
    // 结果**两边都变成每次都重算**，表现为"一边好好的、另一边每帧卡"。
    for (CacheEntry& e : g_slots) {
        if (SameParams(e, path, dstW, dstH, fit, manual, blurPx, dimPct, opacityPct)) {
            e.used = ++g_tick;
            return e.bmp.valid() ? &e.bmp : nullptr;
        }
    }

    // 没命中：占用**最久没用过**的那个槽
    CacheEntry& slot =
        (g_slots[0].used <= g_slots[1].used) ? g_slots[0] : g_slots[1];

    // 参数变了：先把槽标记为失效，这样中途失败不会留下旧图冒充新参数的结果
    slot.valid = false;
    slot.bmp   = BgBitmap{};
    slot.path  = path;
    slot.dstW  = dstW;
    slot.dstH  = dstH;
    slot.fit    = fit;
    slot.manual = manual;
    slot.blur   = blurPx;
    slot.dim   = dimPct;
    slot.opacity = opacityPct;
    slot.used  = ++g_tick;

    if (!EnsureCom()) {
        g_lastError = L"COM 初始化失败";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    CComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)))) {
        g_lastError = L"拿不到 WIC 工厂";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    int imgW = 0, imgH = 0;
    if (!ReadImageSize(factory, path, imgW, imgH)) {
        g_lastError = L"打不开图片（不存在 / 格式不支持）";
        DebugLog("背景图：%ls —— %ls", g_lastError.c_str(), path.c_str());
        return nullptr;
    }

    const BgPlacement place = ComputeBgPlacement(imgW, imgH, dstW, dstH, fit, manual);
    if (!place.valid) {
        g_lastError = L"图片尺寸或面板尺寸非法";
        DebugLog("背景图：%ls（图 %dx%d -> 目标 %dx%d）",
                 g_lastError.c_str(), imgW, imgH, dstW, dstH);
        return nullptr;
    }

    // ---- 解码 + 转 BGRA + 裁剪 ----
    CComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder))) {
        g_lastError = L"解码失败";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    CComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        g_lastError = L"图片里没有可用的帧";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    // 统一转 32bpp BGRA —— 和 svg_icon 的 RasterIcon、分层渲染的
    // m_layeredBits 同一种布局，三处混起来不用转换。
    CComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        g_lastError = L"转 BGRA 失败";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    CComPtr<IWICBitmapSource> src;
    conv.QueryInterface(&src);

    // 先裁再缩。
    //
    // ⚠️ 顺序不能反：先缩的话缩放器要处理整幅原图（可能 4000x3000），
    //    而 Cover 裁完可能只剩三分之一 —— 白算 2/3 的像素。
    //    裁是零拷贝的元数据操作，放前面等于免费。
    const int srcW = place.src.right - place.src.left;
    const int srcH = place.src.bottom - place.src.top;
    if ((place.src.left != 0 || place.src.top != 0)) {
        CComPtr<IWICBitmapClipper> clipper;
        HRESULT hr = factory->CreateBitmapClipper(&clipper);
        if (SUCCEEDED(hr)) {
            WICRect r{ place.src.left, place.src.top, srcW, srcH };
            hr = clipper->Initialize(conv, &r);
        }
        if (FAILED(hr)) {
            g_lastError = L"裁剪失败";
            DebugLog("背景图：%ls", g_lastError.c_str());
            return nullptr;
        }
        src = clipper;
    }

    // ---- 缩放 ----
    // Tile 不缩放（原尺寸平铺是它的语义）；其余按 place.dst 的尺寸缩。
    const int drawW = (place.tile ? srcW : place.dst.right - place.dst.left);
    const int drawH = (place.tile ? srcH : place.dst.bottom - place.dst.top);
    if (drawW <= 0 || drawH <= 0) {
        g_lastError = L"缩放目标为空";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    CComPtr<IWICBitmapSource> scaled;
    if (drawW != srcW || drawH != srcH) {
        CComPtr<IWICBitmapScaler> scaler;
        HRESULT hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) {
            // Fant：高质量插值。背景图只在参数变化时缩一次，
            // 用便宜的 Fant 会在缩得很小时出现明显的锯齿/摩尔纹。
            hr = scaler->Initialize(src, static_cast<UINT>(drawW), static_cast<UINT>(drawH),
                                    WICBitmapInterpolationModeFant);
        }
        if (FAILED(hr)) {
            g_lastError = L"缩放失败";
            DebugLog("背景图：%ls", g_lastError.c_str());
            return nullptr;
        }
        scaled = scaler;
    } else {
        scaled = src;
    }

    // ---- 取像素 ----
    //
    // ⚠️ 中间缓冲**封顶**（D-105）。手动缩放最大 400%，而 base 本身就可能
    //    很大（一张小图铺满大面板），两者相乘能到几万像素见方 ——
    //    那个缓冲要几百 MB。32 位进程直接分配失败，64 位也会把面板拖垮。
    //    超了就当作"读不到图"并如实记原因，而不是硬着头皮分下去。
    const size_t pixelBytes = static_cast<size_t>(drawW) * drawH * 4;
    constexpr size_t kMaxPixelBytes = 512u * 1024u * 1024u;
    if (pixelBytes > kMaxPixelBytes) {
        g_lastError = L"图片缩放后过大，已跳过";
        DebugLog("背景图：%ls —— %dx%d（%llu MB），上限 %llu MB",
                 g_lastError.c_str(), drawW, drawH,
                 static_cast<unsigned long long>(pixelBytes / (1024 * 1024)),
                 static_cast<unsigned long long>(kMaxPixelBytes / (1024 * 1024)));
        return nullptr;
    }

    std::vector<unsigned char> pixels(pixelBytes);
    const UINT stride = static_cast<UINT>(drawW) * 4;
    if (FAILED(scaled->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                  pixels.data()))) {
        g_lastError = L"读取像素失败";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    // ---- 铺到目标尺寸的缓冲里 ----
    //
    // 输出缓冲**等于目标区域大小**（不是图的大小）：图和透明区都摆好，
    // 绘制侧只剩"贴上去"一件事 —— 分层路径一次 memcpy，非分层一次 StretchDIBits。
    // 让绘制侧去算摆放的话，那段逻辑就得在两条渲染路径里各写一遍。
    BgBitmap bmp;
    bmp.width  = dstW;
    bmp.height = dstH;
    bmp.bgra.assign(static_cast<size_t>(dstW) * dstH * 4, 0);

    const int offX = place.tile ? 0 : place.dst.left;
    const int offY = place.tile ? 0 : place.dst.top;

    // ⚠️ 这一行是 D-105 那次崩溃的**取证点**。
    //    崩在 BlitInto 里读越界，但崩溃报告连着两次只有几字节（二次崩溃）、
    //    拿不到局部变量 —— 所以把实际数值打出来：
    //    下一次真出问题，日志里直接能看到是哪个尺寸/偏移不对。
    //    只在参数变化（也就是真的重算）时记一行，不会刷屏。
    DebugLog("背景图 BlitInto: dst=%dx%d src=%dx%d off=(%d,%d) tile=%d fit=%d",
             dstW, dstH, drawW, drawH, offX, offY,
             place.tile ? 1 : 0, static_cast<int>(fit));

    BlitInto(bmp.bgra, dstW, dstH, pixels.data(), drawW, drawH, offX, offY, place.tile);

    // ---- 模糊 ----
    //
    // ⚠️ 只在**图占的那块**上模糊，不模糊整个缓冲。
    //    整块模糊的话，Contain 留下的透明区（RGB 全 0）会被卷进来，
    //    图片四周糊出一圈暗晕 —— 那看起来像"图有黑边"。
    //    传子矩形指针即可：BoxBlurBgra 内部所有索引都夹在它自己的 w/h 内，
    //    不会读到区域外面去。
    if (blurPx > 0) {
        const int bw = place.tile ? dstW : drawW;
        const int bh = place.tile ? dstH : drawH;
        unsigned char* base = bmp.bgra.data() +
                              (static_cast<size_t>(offY) * dstW + offX) * 4;
        BoxBlurBgra(base, bw, bh, blurPx);
    }

    // ---- 压暗 + 不透明度 ----
    // 同样只作用在图占的那块上：透明区改不改都看不见，
    // 但少扫一遍就是少几百万次乘法。
    if (dimPct != 0 || opacityPct != 100) {
        const int bw = place.tile ? dstW : drawW;
        const int bh = place.tile ? dstH : drawH;
        unsigned char* base = bmp.bgra.data() +
                              (static_cast<size_t>(offY) * dstW + offX) * 4;
        // ApplyDimAndOpacity 按行连续处理，所以这里要逐行走（缓冲的
        // stride 是 dstW，而图可能只占其中一段）。
        if (bw == dstW) {
            ApplyDimAndOpacity(base, bw, bh, dimPct, opacityPct);
        } else {
            for (int y = 0; y < bh; ++y) {
                ApplyDimAndOpacity(base + static_cast<size_t>(y) * dstW * 4, bw, 1,
                                   dimPct, opacityPct);
            }
        }
    }

    if (!bmp.valid()) {
        g_lastError = L"结果缓冲非法";
        DebugLog("背景图：%ls", g_lastError.c_str());
        return nullptr;
    }

    slot.bmp   = std::move(bmp);
    slot.valid = true;

    DebugLog("背景图：已加载 %ls（图 %dx%d -> 面板 %dx%d，%ls 模糊=%d 压暗=%d 不透明=%d）",
             path.c_str(), imgW, imgH, dstW, dstH,
             place.tile ? L"平铺" : L"缩放", blurPx, dimPct, opacityPct);

    return &slot.bmp;
}

bool GetBgImageSize(const std::wstring& path, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (path.empty() || !EnsureCom()) return false;

    CComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory)))) {
        return false;
    }
    return ReadImageSize(factory, path, outW, outH);
}

void ClearPanelBackgroundCache() {
    // 两个槽一起清 —— 面板和预览用的是同一个图片文件时，只清一个会让
    // 另一个留着旧图，表现为"点了清除但还看得见"。
    g_slots[0] = CacheEntry{};
    g_slots[1] = CacheEntry{};
    g_tick = 0;
    g_lastError.clear();
}

const wchar_t* LastBgError() {
    return g_lastError.c_str();
}

} // namespace lyricus
