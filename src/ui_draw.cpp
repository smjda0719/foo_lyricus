#include "stdafx.h"

#include "ui_draw.h"

#include <SDK/ui_element.h>   // ui_config_manager：宿主主题色 + 暗色模式

// ---------------------------------------------------------------------------
// 自绘界面共用的小工具。见 ui_draw.h 里"为什么抽出来"。
// ---------------------------------------------------------------------------

namespace lyricus {

HostTheme QueryHostTheme() {
    HostTheme t;

    ui_config_manager::ptr cm = ui_config_manager::tryGet();
    if (cm.is_valid()) {
        t.bg   = cm->getSysColor(COLOR_WINDOW);
        t.fg   = cm->getSysColor(COLOR_WINDOWTEXT);
        t.dark = cm->is_dark_mode();
    } else {
        // 拿不到服务（老版本 foobar2000 或无 GUI）：退回系统色。
        // 这条路上没有暗色模式的概念，dark 保持 false 即可。
        t.bg = GetSysColor(COLOR_WINDOW);
        t.fg = GetSysColor(COLOR_WINDOWTEXT);
    }
    return t;
}

COLORREF BlendColor(COLORREF a, COLORREF b, double t) {
    auto mix = [t](int x, int y) {
        int v = static_cast<int>(x * (1.0 - t) + y * t + 0.5);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return v;
    };
    return RGB(mix(GetRValue(a), GetRValue(b)),
               mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) return;

    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    if (radius < 1) {   // 太扁了，退化成直角
        HBRUSH br = CreateSolidBrush(color);
        FillRect(dc, &r, br);
        DeleteObject(br);
        return;
    }

    HBRUSH br = CreateSolidBrush(color);
    HPEN   pn = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ ob = SelectObject(dc, br);
    const HGDIOBJ op = SelectObject(dc, pn);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

void StrokeRoundRect(HDC dc, const RECT& r, int radius, int width, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) return;

    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius < 1) radius = 1;

    // NULL_BRUSH = 只描边不填充。别用 GetStockObject(HOLLOW_BRUSH) 的名字，
    // 那是同一个东西的另一个叫法，这里写清楚意图。
    HBRUSH br = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN   pn = CreatePen(PS_SOLID, width, color);
    const HGDIOBJ ob = SelectObject(dc, br);
    const HGDIOBJ op = SelectObject(dc, pn);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pn);
}

void DrawTextIn(HDC dc, const RECT& r, const wchar_t* text, COLORREF color,
                HFONT font, UINT flags) {
    if (text == nullptr || *text == L'\0') return;
    if (font == nullptr) return;

    const HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT rc = r;
    DrawTextW(dc, text, -1, &rc, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

HFONT MakeUiFont(int dpi, int pt, bool semibold) {
    if (dpi <= 0) dpi = 96;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    LOGFONTW lf{};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        // 兜底：系统不给就自己拼一个，别让整页画不出字
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }
    lf.lfHeight  = -MulDiv(pt, dpi, 72);
    lf.lfWeight  = semibold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&lf);
}

} // namespace lyricus
