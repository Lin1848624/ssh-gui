// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  widgets.cpp
// ===========================================================================
#include "widgets.h"

#include <dwmapi.h>
#include <map>
#include <string>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  字体缓存
// ---------------------------------------------------------------------------
namespace {

struct FontKey {
    int  px;
    bool mono;
    bool bold;
    bool operator<(const FontKey& o) const {
        if (px != o.px) return px < o.px;
        if (mono != o.mono) return mono < o.mono;
        return bold < o.bold;
    }
};

std::map<FontKey, Font*>& FontCache() {
    static std::map<FontKey, Font*> cache;
    return cache;
}

Font* MakeFont(const wchar_t* face, const wchar_t* fallback, int px, bool bold) {
    Font* f = new Font(face, (REAL)px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    if (f->GetLastStatus() != Ok) {
        delete f;
        f = new Font(fallback, (REAL)px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    }
    return f;
}

} // namespace

HFONT Gfx::MakeUiFont(int px, bool bold) {
    LOGFONTW lf = {};
    lf.lfHeight = -px;
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, Theme::UiFont, LF_FACESIZE);
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) {
        lstrcpynW(lf.lfFaceName, Theme::UiFontAlt, LF_FACESIZE);
        f = CreateFontIndirectW(&lf);
    }
    return f;
}

Font* Gfx::UiFont(int px, bool bold) {
    FontKey k{ px, false, bold };
    auto it = FontCache().find(k);
    if (it != FontCache().end()) return it->second;

    Font* f = MakeFont(Theme::UiFont, Theme::UiFontAlt, px, bold);
    FontCache()[k] = f;
    return f;
}

Font* Gfx::MonoFont(int px, bool bold) {
    FontKey k{ px, true, bold };
    auto it = FontCache().find(k);
    if (it != FontCache().end()) return it->second;

    Font* f = MakeFont(Theme::MonoFont, Theme::MonoFontAlt, px, bold);
    FontCache()[k] = f;
    return f;
}

// ---------------------------------------------------------------------------
//  基本图形
// ---------------------------------------------------------------------------
void Gfx::RoundRectPath(GraphicsPath& path, const RectF& r, float radius) {
    if (radius <= 0.1f) {
        path.AddRectangle(r);
        return;
    }
    float d = radius * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    float d2 = d / 2.0f;

    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    (void)d2;
}

void Gfx::FillRound(Graphics& g, const RectF& r, float radius, uint32_t color) {
    GraphicsPath path;
    RoundRectPath(path, r, radius);
    SolidBrush br(GC(color));
    g.FillPath(&br, &path);
}

void Gfx::StrokeRound(Graphics& g, const RectF& r, float radius, uint32_t color, float width) {
    GraphicsPath path;
    RoundRectPath(path, r, radius);
    Pen pen(GC(color), width);
    g.DrawPath(&pen, &path);
}

void Gfx::FillRectC(Graphics& g, const RectF& r, uint32_t color) {
    SolidBrush br(GC(color));
    g.FillRectangle(&br, r);
}

void Gfx::Line(Graphics& g, float x1, float y1, float x2, float y2, uint32_t color, float width) {
    Pen pen(GC(color), width);
    g.DrawLine(&pen, x1, y1, x2, y2);
}

SizeF Gfx::MeasureText(Graphics& g, const std::wstring& s, Font* f) {
    RectF layout(0, 0, 4096.0f, 4096.0f);
    RectF out;
    g.MeasureString(s.c_str(), (INT)s.size(), f, layout, nullptr, &out);
    return SizeF(out.Width, out.Height);
}

void Gfx::Text(Graphics& g, const std::wstring& s, Font* f, uint32_t color,
               const RectF& r, int align, int valign, bool ellipsis) {
    if (s.empty()) return;

    StringFormat fmt;
    fmt.SetAlignment(align == 0 ? StringAlignmentNear
                   : align == 2 ? StringAlignmentFar
                                : StringAlignmentCenter);
    fmt.SetLineAlignment(valign == 0 ? StringAlignmentNear
                        : valign == 2 ? StringAlignmentFar
                                      : StringAlignmentCenter);
    if (ellipsis) fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush br(GC(color));
    g.DrawString(s.c_str(), (INT)s.size(), f, r, &fmt, &br);
}

// ---------------------------------------------------------------------------
//  按钮悬停跟踪
// ---------------------------------------------------------------------------
namespace {

constexpr UINT_PTR kSubclassId = 0x5A61;
const wchar_t* const kHoverProp = L"SshGuiBtnHover";

LRESULT CALLBACK ButtonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kHoverProp)) {
            SetPropW(h, kHoverProp, (HANDLE)1);
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (GetPropW(h, kHoverProp)) {
            RemovePropW(h, kHoverProp);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        RemovePropW(h, kHoverProp);
        RemoveWindowSubclass(h, ButtonSubclass, id);
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

} // namespace

void TrackButtonHover(HWND h) {
    if (!h) return;
    SetWindowSubclass(h, ButtonSubclass, kSubclassId, 0);
}

bool ButtonHovered(HWND h) {
    return h && GetPropW(h, kHoverProp) != nullptr;
}

// ---------------------------------------------------------------------------
//  深色标题栏
// ---------------------------------------------------------------------------
void ApplyDarkTitleBar(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+)；旧版是 19
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
}

// ---------------------------------------------------------------------------
//  按钮绘制
// ---------------------------------------------------------------------------
std::wstring ButtonText(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return std::wstring();
    std::wstring s((size_t)len + 1, L'\0');
    GetWindowTextW(h, &s[0], len + 1);
    s.resize((size_t)len);
    return s;
}

void DrawButton(const DRAWITEMSTRUCT* dis, bool hovered, BtnStyle style) {
    if (!dis) return;

    HWND h = dis->hwndItem;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool focused  = (dis->itemState & ODS_FOCUS) != 0;

    float w = (float)(rc.right - rc.left);
    float ht = (float)(rc.bottom - rc.top);
    if (w <= 0 || ht <= 0) return;

    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // 背景底色（父窗口色）
    Gfx::FillRectC(g, RectF(0.0f, 0.0f, w, ht), Theme::PanelBg);

    RectF r(1.0f, 1.0f, w - 2.0f, ht - 2.0f);
    float radius = (float)S(6);

    uint32_t bg = Theme::BtnBg;
    uint32_t fg = Theme::Text;

    switch (style) {
    case BtnStyle::Primary:
        bg = pressed ? Theme::AccentDim : (hovered ? Theme::AccentHover : Theme::Accent);
        fg = 0xFFFFFF;
        break;
    case BtnStyle::Danger:
        bg = pressed ? 0x7A1F1F : (hovered ? 0x9E2B2B : 0x5C1A1A);
        fg = hovered ? 0xFFE1E1 : 0xE8A0A0;
        break;
    case BtnStyle::Ghost:
        bg = pressed ? Theme::BtnPress : (hovered ? Theme::BtnHover : Theme::PanelBg);
        fg = hovered ? Theme::Text : Theme::TextDim;
        break;
    default:
        bg = pressed ? Theme::BtnPress : (hovered ? Theme::BtnHover : Theme::BtnBg);
        fg = Theme::Text;
        break;
    }

    if (disabled) {
        bg = 0x24262B;
        fg = Theme::TextFaint;
    }

    Gfx::FillRound(g, r, radius, bg);
    if (style == BtnStyle::Normal || style == BtnStyle::Ghost) {
        Gfx::StrokeRound(g, r, radius, disabled ? 0x2A2D33 : Theme::BorderLight, 1.0f);
    }

    std::wstring text = ButtonText(h);
    if (!text.empty()) {
        Font* f = Gfx::UiFont(S(13), false);
        RectF tr(r.X + (REAL)S(6), r.Y, r.Width - (REAL)S(12), r.Height);
        Gfx::Text(g, text, f, fg, tr, 1, 1);
    }

    if (focused && !disabled) {
        Gfx::StrokeRound(g, RectF(r.X + 1.5f, r.Y + 1.5f, r.Width - 3.0f, r.Height - 3.0f),
                         radius - 1.0f, Theme::AccentHover, 1.0f);
    }

    g.Flush(FlushIntentionSync);
}
