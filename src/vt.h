// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  vt.h - VT/ANSI 终端模拟器
//  只做"字节流 -> 字符网格"这件事，不碰任何窗口或绘图。
// ===========================================================================
#pragma once

#include "common.h"
#include "theme.h"

#include <deque>
#include <string>
#include <vector>

// 单元格属性位
enum : uint16_t {
    A_Bold      = 1 << 0,
    A_Dim       = 1 << 1,
    A_Italic    = 1 << 2,
    A_Underline = 1 << 3,
    A_Blink     = 1 << 4,
    A_Reverse   = 1 << 5,
    A_Hidden    = 1 << 6,
    A_Strike    = 1 << 7,
    A_Wide      = 1 << 8,   // 宽字符首格
    A_WideTail  = 1 << 9,   // 宽字符第二格（不画字形，只画背景）
};

struct Cell {
    uint32_t ch   = ' ';
    uint32_t fg   = Theme::ColorDefaultFg;
    uint32_t bg   = Theme::ColorDefaultBg;
    uint16_t attr = 0;
};

using Row = std::vector<Cell>;

// 码点显示宽度：0 = 组合字符，2 = 全角/宽
int CodepointWidth(uint32_t cp);

class VtTerminal {
public:
    VtTerminal();
    ~VtTerminal();

    // ---- 输入 ----
    void Feed(const char* data, size_t len);
    void Resize(int cols, int rows);
    void Reset(bool clearScrollback = true);

    // ---- 尺寸 ----
    int Cols() const { return m_cols; }
    int Rows() const { return m_rows; }

    // ---- 取字符 ----
    const Cell& At(int row, int col) const;                 // row: 0 = 可见区顶部
    const Cell& CellAtAbs(int absRow, int col) const;       // absRow: 0 = 回看最旧一行

    int ScrollbackCount() const { return (int)m_scrollback.size(); }
    int TotalRows() const { return (int)m_scrollback.size() + m_rows; }

    // ---- 状态 ----
    int  CursorX() const { return m_cursorX; }
    int  CursorY() const { return m_cursorY; }
    bool CursorVisible() const { return m_cursorVisible; }
    bool CursorBlink() const { return m_cursorBlink; }
    bool AppCursorKeys() const { return m_appCursorKeys; }
    bool AppKeypad() const { return m_appKeypad; }
    bool BracketedPaste() const { return m_bracketedPaste; }
    bool AltScreen() const { return m_altActive; }
    int  MouseMode() const { return m_mouseMode; }          // 0/1000/1002/1003
    bool MouseSgr() const { return m_mouseSgr; }

    // ---- 一次性事件 ----
    const std::wstring& Title() const { return m_title; }
    bool TakeTitleDirty() { bool d = m_titleDirty; m_titleDirty = false; return d; }
    bool TakeBell() { bool b = m_bell; m_bell = false; return b; }
    bool TakeBellFlash() { bool b = m_bellFlash; m_bellFlash = false; return b; }
    std::wstring TakeOscClipboard();

    // ---- 文本提取（选区复制用）----
    std::wstring RowText(int absRow, bool trimRight = true) const;
    std::wstring RangeText(int absRow1, int col1, int absRow2, int col2) const;

private:
    enum class St {
        Ground, Escape, EscapeIntermediate,
        CsiEntry, CsiParam, CsiIntermediate, CsiIgnore,
        OscString, DcsPassthrough, SosPmApc,
    };

    struct SavedCursor {
        int      x = 0, y = 0;
        uint32_t fg = Theme::ColorDefaultFg;
        uint32_t bg = Theme::ColorDefaultBg;
        uint16_t attr = 0;
        bool     originMode = false;
        int      g[2] = { 0, 0 };
        int      activeG = 0;
        bool     wrapPending = false;
    };

    // ---- 屏幕管理 ----
    Row  MakeBlankRow() const;
    Cell MakeBlankCell() const;
    void InitScreen(std::vector<Row>& scr) const;
    void ClearScreenArea(std::vector<Row>& scr);

    // ---- 编辑 ----
    void PutChar(uint32_t cp);
    void ExecuteC0(unsigned char c);
    void CarriageReturn();
    void LineFeed();
    void ReverseIndex();
    void Tab();
    void Backspace();
    void ScrollUpRegion(int count);
    void ScrollDownRegion(int count);

    void EraseInDisplay(int mode);
    void EraseInLine(int mode);
    void InsertChars(int n);
    void DeleteChars(int n);
    void EraseChars(int n);
    void InsertLines(int n);
    void DeleteLines(int n);

    void SetCursor(int row, int col);
    void MoveCursor(int dx, int dy);
    void SaveCursor();
    void RestoreCursor();

    void ApplySgr(const std::vector<int>& p);
    void ResetAttrs();

    // ---- 解析 ----
    void ProcessAscii(unsigned char c);
    void ProcessCodepoint(uint32_t cp);
    void HandleEsc(unsigned char c);
    void HandleCsi(unsigned char c);
    void CsiDispatch(wchar_t final);
    void HandleStringByte(unsigned char c);
    void EndString();
    void OscDispatch();

    uint32_t MapCharset(uint32_t cp) const;
    int Param(size_t idx, int def, int minV = 1) const;

    void SwitchAltScreen(bool enable);
    void ResetParser();

    // ---- 数据 ----
    int m_cols = 80;
    int m_rows = 24;

    std::vector<Row> m_screen;
    std::vector<Row> m_mainSaved;      // 进备用屏时暂存主屏
    std::deque<Row>  m_scrollback;
    size_t           m_scrollbackMax = 8000;

    bool m_altActive = false;

    int  m_cursorX = 0;
    int  m_cursorY = 0;
    bool m_cursorVisible = true;
    bool m_cursorBlink   = true;
    bool m_wrapPending   = false;
    bool m_autoWrap      = true;
    bool m_originMode    = false;
    bool m_insertMode    = false;
    bool m_appCursorKeys = false;
    bool m_appKeypad     = false;
    bool m_bracketedPaste = false;
    int  m_mouseMode     = 0;
    bool m_mouseSgr      = false;

    int m_scrollTop    = 0;
    int m_scrollBottom = 23;

    uint32_t m_fg = Theme::ColorDefaultFg;
    uint32_t m_bg = Theme::ColorDefaultBg;
    uint16_t m_attr = 0;

    SavedCursor m_saved;
    SavedCursor m_savedAlt;

    int m_charsetG[2] = { 0, 0 };   // 0 = ASCII, 1 = DEC 特殊图形
    int m_activeCharset = 0;
    bool m_singleShift = false;

    // ---- 解析状态 ----
    St               m_st = St::Ground;
    uint32_t         m_utf8Acc = 0;
    int              m_utf8Need = 0;
    int              m_utf8Have = 0;
    std::vector<int> m_params;
    bool             m_private = false;
    wchar_t          m_csiIntermediate = 0;
    wchar_t          m_escIntermediate = 0;
    std::string      m_strBuf;
    bool             m_strEsc = false;

    std::wstring m_title;
    bool         m_titleDirty = false;
    bool         m_bell = false;
    bool         m_bellFlash = false;
    std::string  m_oscClipboard;
};
