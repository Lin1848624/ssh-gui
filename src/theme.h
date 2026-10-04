// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  theme.h - 深色主题配色与字体
// ===========================================================================
#pragma once

#include "common.h"
#include <gdiplus.h>

// 颜色统一用 0xRRGGBB 表示，渲染前再转 COLORREF / Gdiplus::Color
inline COLORREF Rgb(uint32_t v) {
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}
inline Gdiplus::Color GC(uint32_t v, BYTE a = 255) {
    return Gdiplus::Color(a, (BYTE)((v >> 16) & 0xFF), (BYTE)((v >> 8) & 0xFF), (BYTE)(v & 0xFF));
}
inline COLORREF GCr(uint32_t v) { return Rgb(v); }

namespace Theme {

// ---- 界面 ----
constexpr uint32_t Bg          = 0x1B1D22;   // 窗口底
constexpr uint32_t PanelBg     = 0x22252B;   // 面板 / 工具条
constexpr uint32_t SidebarBg    = 0x17191E;  // 会话侧边栏
constexpr uint32_t TermBg      = 0x101216;   // 终端默认背景
constexpr uint32_t TermFg      = 0xD8DEE9;   // 终端默认前景
constexpr uint32_t Border      = 0x2E323A;
constexpr uint32_t BorderLight = 0x3A3F49;

constexpr uint32_t Text        = 0xE4E8EF;
constexpr uint32_t TextDim     = 0x8A93A3;
constexpr uint32_t TextFaint   = 0x5C6473;

constexpr uint32_t Accent      = 0x2F7FE8;
constexpr uint32_t AccentHover = 0x4A93F0;
constexpr uint32_t AccentDim   = 0x1E4E8C;

constexpr uint32_t BtnBg       = 0x2C3038;
constexpr uint32_t BtnHover    = 0x383D47;
constexpr uint32_t BtnPress    = 0x24272E;

constexpr uint32_t EditBg      = 0x16181D;
constexpr uint32_t EditBorder  = 0x3A3F49;

constexpr uint32_t TabActive   = 0x22252B;
constexpr uint32_t TabIdle     = 0x17191E;
constexpr uint32_t TabHover    = 0x1E2127;

constexpr uint32_t Ok          = 0x3FB950;
constexpr uint32_t Warn        = 0xD29922;
constexpr uint32_t Err         = 0xF85149;

constexpr uint32_t SelectionBg = 0x2A4A7F;   // 终端选区高亮

// ---- 终端 ANSI 16 色（Campbell 方案，与 Windows Terminal 默认一致）----
constexpr uint32_t Ansi[16] = {
    0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00,
    0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
    0x767676, 0xE74856, 0x16C60C, 0xF9F1A5,
    0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2,
};

// ---- 终端里的特殊颜色标记 ----
constexpr uint32_t ColorDefaultFg = 0xFFFFFFFF;   // 未指定，用主题前景
constexpr uint32_t ColorDefaultBg = 0xFFFFFFFE;   // 未指定，用主题背景

// ---- 字体名 ----
constexpr wchar_t UiFont[]   = L"Microsoft YaHei UI";
constexpr wchar_t UiFontAlt[] = L"Microsoft Sans Serif";
constexpr wchar_t MonoFont[] = L"Consolas";
constexpr wchar_t MonoFontAlt[] = L"Courier New";

} // namespace Theme
