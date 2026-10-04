// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  widgets.h - GDI+ 绘图辅助与自绘按钮
// ===========================================================================
#pragma once

#include "common.h"
#include "theme.h"

#include <gdiplus.h>

namespace Gfx {

// 系统控件用的 GDI 字体（EDIT / LISTBOX 这些要靠 HFONT）
HFONT MakeUiFont(int px, bool bold = false);

// 字体缓存（按 像素高度+粗体 缓存，程序退出时统一回收）
Gdiplus::Font* UiFont(int px, bool bold = false);
Gdiplus::Font* MonoFont(int px, bool bold = false);

void RoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);
void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, uint32_t color);
void StrokeRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                 uint32_t color, float width);
void FillRectC(Gdiplus::Graphics& g, const Gdiplus::RectF& r, uint32_t color);
void Line(Gdiplus::Graphics& g, float x1, float y1, float x2, float y2,
          uint32_t color, float width = 1.0f);

// align: 0=左 1=中 2=右；valign: 0=上 1=中 2=下
void Text(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f, uint32_t color,
          const Gdiplus::RectF& r, int align = 0, int valign = 1, bool ellipsis = true);
Gdiplus::SizeF MeasureText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f);

} // namespace Gfx

// ---------------------------------------------------------------------------
//  按钮
// ---------------------------------------------------------------------------
enum class BtnStyle { Normal, Primary, Danger, Ghost };

bool ButtonHovered(HWND h);
void TrackButtonHover(HWND h);

// 标题栏跟随深色主题。必须在 CreateWindowEx 返回之后再调，
// 在 WM_CREATE 里调往往太早、DWM 还不认这个窗口。
void ApplyDarkTitleBar(HWND hwnd);

void DrawButton(const DRAWITEMSTRUCT* dis, bool hovered, BtnStyle style = BtnStyle::Normal);

// 取按钮文字
std::wstring ButtonText(HWND h);
