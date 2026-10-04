// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  vt.cpp - VT/ANSI 解析与字符网格维护
// ===========================================================================
#include "vt.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

// ---------------------------------------------------------------------------
//  Unicode 显示宽度
// ---------------------------------------------------------------------------
static bool IsCombining(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||   // 组合变音符号
           (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05BD) ||
           (cp >= 0x0610 && cp <= 0x061A) ||
           (cp >= 0x064B && cp <= 0x065F) ||
           (cp >= 0x0670 && cp <= 0x0670) ||
           (cp >= 0x06D6 && cp <= 0x06DC) ||
           (cp >= 0x0730 && cp <= 0x074A) ||
           (cp >= 0x07A6 && cp <= 0x07B0) ||
           (cp >= 0x0900 && cp <= 0x0903) ||
           (cp >= 0x093A && cp <= 0x094F) ||
           (cp >= 0x0951 && cp <= 0x0957) ||
           (cp >= 0x0E31 && cp <= 0x0E3A) ||
           (cp >= 0x0E47 && cp <= 0x0E4E) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x200B && cp <= 0x200F) ||   // 零宽字符
           (cp >= 0x2060 && cp <= 0x2064) ||
           (cp >= 0x20D0 && cp <= 0x20F0) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||   // 变体选择符
           (cp >= 0xFE20 && cp <= 0xFE2F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

static bool IsWide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||   // 韩文字母
           (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK 部首 / 标点
           (cp >= 0x3041 && cp <= 0x33FF) ||   // 假名 / 注音 / CJK 兼容
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK 扩展 A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK 基本区
           (cp >= 0xA000 && cp <= 0xA4CF) ||   // 彝文
           (cp >= 0xA960 && cp <= 0xA97F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // 韩文音节
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK 兼容表意
           (cp >= 0xFE10 && cp <= 0xFE19) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) ||
           (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1F64F) || // Emoji
           (cp >= 0x1F900 && cp <= 0x1F9FF) ||
           (cp >= 0x1FA70 && cp <= 0x1FAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);
}

int CodepointWidth(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20) return 0;
    if (cp < 0x7F) return 1;
    if (cp < 0xA0) return 0;             // DEL 与 C1 控制符
    if (IsCombining(cp)) return 0;
    if (IsWide(cp)) return 2;
    return 1;
}

static void AppendUtf16(std::wstring& out, uint32_t cp) {
    if (cp < 0x10000) {
        out.push_back((wchar_t)cp);
    } else {
        uint32_t v = cp - 0x10000;
        out.push_back((wchar_t)(0xD800 + (v >> 10)));
        out.push_back((wchar_t)(0xDC00 + (v & 0x3FF)));
    }
}

static const Cell kBlankCell{};

// ---------------------------------------------------------------------------
//  构造 / 重置 / 尺寸
// ---------------------------------------------------------------------------
VtTerminal::VtTerminal() {
    InitScreen(m_screen);
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
}

VtTerminal::~VtTerminal() = default;

Cell VtTerminal::MakeBlankCell() const {
    Cell c;
    c.ch = L' ';
    c.fg = Theme::ColorDefaultFg;
    c.bg = m_bg;                 // BCE：新空白沿用当前背景色
    c.attr = 0;
    return c;
}

Row VtTerminal::MakeBlankRow() const {
    Row r((size_t)m_cols, MakeBlankCell());
    return r;
}

void VtTerminal::InitScreen(std::vector<Row>& scr) const {
    scr.assign((size_t)m_rows, Row());
    for (auto& r : scr) r.assign((size_t)m_cols, MakeBlankCell());
}

void VtTerminal::ClearScreenArea(std::vector<Row>& scr) {
    Cell blank = MakeBlankCell();
    for (auto& r : scr) {
        r.assign((size_t)m_cols, blank);
    }
}

void VtTerminal::Reset(bool clearScrollback) {
    if (clearScrollback) m_scrollback.clear();

    m_altActive = false;
    ClearScreenArea(m_screen);

    m_cursorX = 0;
    m_cursorY = 0;
    m_cursorVisible = true;
    m_cursorBlink = true;
    m_wrapPending = false;
    m_autoWrap = true;
    m_originMode = false;
    m_insertMode = false;
    m_appCursorKeys = false;
    m_appKeypad = false;
    m_bracketedPaste = false;
    m_mouseMode = 0;
    m_mouseSgr = false;

    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;

    ResetAttrs();

    m_charsetG[0] = m_charsetG[1] = 0;
    m_activeCharset = 0;
    m_singleShift = false;
    m_saved = SavedCursor();
    m_savedAlt = SavedCursor();

    m_title.clear();
    m_titleDirty = true;
    m_oscClipboard.clear();
    ResetParser();
}

void VtTerminal::ResetParser() {
    m_st = St::Ground;
    m_utf8Acc = 0;
    m_utf8Need = 0;
    m_utf8Have = 0;
    m_params.clear();
    m_private = false;
    m_csiIntermediate = 0;
    m_escIntermediate = 0;
    m_strBuf.clear();
    m_strEsc = false;
}

void VtTerminal::ResetAttrs() {
    m_fg = Theme::ColorDefaultFg;
    m_bg = Theme::ColorDefaultBg;
    m_attr = 0;
}

void VtTerminal::Resize(int cols, int rows) {
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols == m_cols && rows == m_rows) return;

    std::vector<Row> old = m_screen;
    int oldRows = m_rows;
    int oldCols = m_cols;

    m_cols = cols;
    m_rows = rows;
    InitScreen(m_screen);

    // 尽量把原内容搬到左上角
    int copyRows = (oldRows < rows) ? oldRows : rows;
    for (int r = 0; r < copyRows; ++r) {
        int copyCols = (oldCols < cols) ? oldCols : cols;
        for (int c = 0; c < copyCols; ++c) {
            if (c < (int)old[r].size()) m_screen[r][c] = old[r][c];
        }
    }

    // 备用屏暂存区尺寸跟着调整
    if (m_altActive) {
        std::vector<Row> saved = m_mainSaved;
        InitScreen(m_mainSaved);
        int sr = ((int)saved.size() < rows) ? (int)saved.size() : rows;
        for (int r = 0; r < sr; ++r) {
            int sc = ((int)saved[r].size() < cols) ? (int)saved[r].size() : cols;
            for (int c = 0; c < sc; ++c) m_mainSaved[r][c] = saved[r][c];
        }
    }

    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;

    if (m_cursorX >= m_cols) m_cursorX = m_cols - 1;
    if (m_cursorY >= m_rows) m_cursorY = m_rows - 1;
    if (m_cursorX < 0) m_cursorX = 0;
    if (m_cursorY < 0) m_cursorY = 0;
    m_wrapPending = false;
}

// ---------------------------------------------------------------------------
//  取字符
// ---------------------------------------------------------------------------
const Cell& VtTerminal::At(int row, int col) const {
    if (row < 0 || row >= (int)m_screen.size()) return kBlankCell;
    const Row& r = m_screen[(size_t)row];
    if (col < 0 || col >= (int)r.size()) return kBlankCell;
    return r[(size_t)col];
}

const Cell& VtTerminal::CellAtAbs(int absRow, int col) const {
    int sb = (int)m_scrollback.size();
    if (absRow < 0) return kBlankCell;
    if (absRow < sb) {
        const Row& r = m_scrollback[(size_t)absRow];
        if (col < 0 || col >= (int)r.size()) return kBlankCell;
        return r[(size_t)col];
    }
    return At(absRow - sb, col);
}

// ---------------------------------------------------------------------------
//  文本提取
// ---------------------------------------------------------------------------
std::wstring VtTerminal::RowText(int absRow, bool trimRight) const {
    std::wstring out;
    out.reserve((size_t)m_cols);

    for (int c = 0; c < m_cols; ++c) {
        const Cell& cell = CellAtAbs(absRow, c);
        if (cell.attr & A_WideTail) continue;
        uint32_t ch = cell.ch ? cell.ch : (uint32_t)L' ';
        AppendUtf16(out, ch);
    }

    if (trimRight) {
        while (!out.empty() && (out.back() == L' ' || out.back() == L'\t')) out.pop_back();
    }
    return out;
}

std::wstring VtTerminal::RangeText(int absRow1, int col1, int absRow2, int col2) const {
    if (absRow1 > absRow2 || (absRow1 == absRow2 && col1 > col2)) {
        std::swap(absRow1, absRow2);
        std::swap(col1, col2);
    }
    if (absRow1 < 0) absRow1 = 0;

    std::wstring out;
    for (int r = absRow1; r <= absRow2; ++r) {
        int c0 = (r == absRow1) ? col1 : 0;
        int c1 = (r == absRow2) ? col2 : m_cols - 1;
        if (c0 < 0) c0 = 0;
        if (c1 > m_cols - 1) c1 = m_cols - 1;

        std::wstring line;
        for (int c = c0; c <= c1; ++c) {
            const Cell& cell = CellAtAbs(r, c);
            if (cell.attr & A_WideTail) continue;
            uint32_t ch = cell.ch ? cell.ch : (uint32_t)L' ';
            AppendUtf16(line, ch);
        }
        if (r != absRow2) {
            while (!line.empty() && line.back() == L' ') line.pop_back();
            out += line;
            out += L"\r\n";
        } else {
            out += line;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  参数辅助
// ---------------------------------------------------------------------------
int VtTerminal::Param(size_t idx, int def, int minV) const {
    if (idx >= m_params.size()) return def;
    int v = m_params[idx];
    if (v < 0) return def;
    if (v < minV) return minV;
    return v;
}

// ---------------------------------------------------------------------------
//  光标
// ---------------------------------------------------------------------------
void VtTerminal::SetCursor(int row, int col) {
    m_wrapPending = false;
    if (m_originMode) {
        row += m_scrollTop;
        if (row > m_scrollBottom) row = m_scrollBottom;
        if (row < m_scrollTop) row = m_scrollTop;
    }
    if (row < 0) row = 0;
    if (row >= m_rows) row = m_rows - 1;
    if (col < 0) col = 0;
    if (col >= m_cols) col = m_cols - 1;
    m_cursorY = row;
    m_cursorX = col;
}

void VtTerminal::MoveCursor(int dx, int dy) {
    m_wrapPending = false;
    int top = m_originMode ? m_scrollTop : 0;
    int bot = m_originMode ? m_scrollBottom : m_rows - 1;

    int nx = m_cursorX + dx;
    int ny = m_cursorY + dy;
    if (nx < 0) nx = 0;
    if (nx >= m_cols) nx = m_cols - 1;
    if (ny < top) ny = top;
    if (ny > bot) ny = bot;
    m_cursorX = nx;
    m_cursorY = ny;
}

void VtTerminal::SaveCursor() {
    SavedCursor& s = m_altActive ? m_savedAlt : m_saved;
    s.x = m_cursorX;
    s.y = m_cursorY;
    s.fg = m_fg;
    s.bg = m_bg;
    s.attr = m_attr;
    s.originMode = m_originMode;
    s.g[0] = m_charsetG[0];
    s.g[1] = m_charsetG[1];
    s.activeG = m_activeCharset;
    s.wrapPending = m_wrapPending;
}

void VtTerminal::RestoreCursor() {
    const SavedCursor& s = m_altActive ? m_savedAlt : m_saved;
    m_cursorX = s.x;
    m_cursorY = s.y;
    m_fg = s.fg;
    m_bg = s.bg;
    m_attr = s.attr;
    m_originMode = s.originMode;
    m_charsetG[0] = s.g[0];
    m_charsetG[1] = s.g[1];
    m_activeCharset = s.activeG;
    m_wrapPending = s.wrapPending;

    if (m_cursorX >= m_cols) m_cursorX = m_cols - 1;
    if (m_cursorY >= m_rows) m_cursorY = m_rows - 1;
    if (m_cursorX < 0) m_cursorX = 0;
    if (m_cursorY < 0) m_cursorY = 0;
}

// ---------------------------------------------------------------------------
//  滚动
// ---------------------------------------------------------------------------
void VtTerminal::ScrollUpRegion(int count) {
    if (count <= 0) return;
    int regionH = m_scrollBottom - m_scrollTop + 1;
    if (count > regionH) count = regionH;

    for (int i = 0; i < count; ++i) {
        if (m_scrollTop == 0 && !m_altActive) {
            m_scrollback.push_back(std::move(m_screen[0]));
            while (m_scrollback.size() > m_scrollbackMax) m_scrollback.pop_front();
            for (int r = 0; r < m_scrollBottom; ++r) {
                m_screen[(size_t)r] = std::move(m_screen[(size_t)r + 1]);
            }
            m_screen[(size_t)m_scrollBottom] = MakeBlankRow();
        } else {
            for (int r = m_scrollTop; r < m_scrollBottom; ++r) {
                m_screen[(size_t)r] = std::move(m_screen[(size_t)r + 1]);
            }
            m_screen[(size_t)m_scrollBottom] = MakeBlankRow();
        }
    }
}

void VtTerminal::ScrollDownRegion(int count) {
    if (count <= 0) return;
    int regionH = m_scrollBottom - m_scrollTop + 1;
    if (count > regionH) count = regionH;

    for (int i = 0; i < count; ++i) {
        for (int r = m_scrollBottom; r > m_scrollTop; --r) {
            m_screen[(size_t)r] = std::move(m_screen[(size_t)r - 1]);
        }
        m_screen[(size_t)m_scrollTop] = MakeBlankRow();
    }
}

// ---------------------------------------------------------------------------
//  基本动作
// ---------------------------------------------------------------------------
void VtTerminal::CarriageReturn() {
    m_cursorX = 0;
    m_wrapPending = false;
}

void VtTerminal::LineFeed() {
    m_wrapPending = false;
    if (m_cursorY == m_scrollBottom) {
        ScrollUpRegion(1);
    } else if (m_cursorY < m_rows - 1) {
        ++m_cursorY;
    }
}

void VtTerminal::ReverseIndex() {
    m_wrapPending = false;
    if (m_cursorY == m_scrollTop) {
        ScrollDownRegion(1);
    } else if (m_cursorY > 0) {
        --m_cursorY;
    }
}

void VtTerminal::Backspace() {
    m_wrapPending = false;
    if (m_cursorX > 0) --m_cursorX;
}

void VtTerminal::Tab() {
    m_wrapPending = false;
    int next = ((m_cursorX / 8) + 1) * 8;
    if (next >= m_cols) next = m_cols - 1;
    m_cursorX = next;
}

uint32_t VtTerminal::MapCharset(uint32_t cp) const {
    int cs = m_singleShift ? m_charsetG[1 - m_activeCharset] : m_charsetG[m_activeCharset];
    if (cs != 1) return cp;
    if (cp < 0x5F || cp > 0x7E) return cp;

    // DEC 特殊图形字符集
    static const uint32_t dec[32] = {
        0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
        0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
        0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
        0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
    };
    return dec[cp - 0x5F];
}

void VtTerminal::PutChar(uint32_t cp) {
    int w = CodepointWidth(cp);
    if (w == 0) {
        // 组合字符：贴到前一个单元格上（不改变宽度），简化处理为忽略
        return;
    }

    if (m_wrapPending) {
        m_wrapPending = false;
        CarriageReturn();
        LineFeed();
    }

    if (w == 2 && m_cursorX + 1 >= m_cols) {
        if (m_autoWrap) {
            CarriageReturn();
            LineFeed();
        } else {
            return;
        }
    }

    if (m_insertMode) InsertChars(w);

    Row& row = m_screen[(size_t)m_cursorY];
    Cell& c = row[(size_t)m_cursorX];
    c.ch = cp;
    c.fg = m_fg;
    c.bg = m_bg;
    c.attr = (uint16_t)(m_attr | (w == 2 ? A_Wide : 0));

    if (w == 2 && m_cursorX + 1 < m_cols) {
        Cell& t = row[(size_t)m_cursorX + 1];
        t.ch = 0;
        t.fg = m_fg;
        t.bg = m_bg;
        t.attr = (uint16_t)(m_attr | A_WideTail);
    }

    m_cursorX += w;
    if (m_cursorX >= m_cols) {
        m_cursorX = m_cols - 1;
        if (m_autoWrap) m_wrapPending = true;
    }
}

// ---------------------------------------------------------------------------
//  擦除 / 插入 / 删除
// ---------------------------------------------------------------------------
void VtTerminal::EraseInDisplay(int mode) {
    Cell blank = MakeBlankCell();
    switch (mode) {
    case 0: {   // 光标 -> 末尾
        Row& row = m_screen[(size_t)m_cursorY];
        for (int c = m_cursorX; c < m_cols; ++c) row[(size_t)c] = blank;
        for (int r = m_cursorY + 1; r < m_rows; ++r) {
            m_screen[(size_t)r].assign((size_t)m_cols, blank);
        }
        break;
    }
    case 1: {   // 开头 -> 光标
        for (int r = 0; r < m_cursorY; ++r) {
            m_screen[(size_t)r].assign((size_t)m_cols, blank);
        }
        Row& row = m_screen[(size_t)m_cursorY];
        for (int c = 0; c <= m_cursorX && c < m_cols; ++c) row[(size_t)c] = blank;
        break;
    }
    case 2:
        ClearScreenArea(m_screen);
        break;
    case 3:
        ClearScreenArea(m_screen);
        m_scrollback.clear();
        break;
    default:
        break;
    }
    m_wrapPending = false;
}

void VtTerminal::EraseInLine(int mode) {
    Cell blank = MakeBlankCell();
    Row& row = m_screen[(size_t)m_cursorY];
    switch (mode) {
    case 0:
        for (int c = m_cursorX; c < m_cols; ++c) row[(size_t)c] = blank;
        break;
    case 1:
        for (int c = 0; c <= m_cursorX && c < m_cols; ++c) row[(size_t)c] = blank;
        break;
    case 2:
        row.assign((size_t)m_cols, blank);
        break;
    default:
        break;
    }
    m_wrapPending = false;
}

void VtTerminal::InsertChars(int n) {
    if (n <= 0 || m_cursorX >= m_cols) return;
    if (m_cursorX + n > m_cols) n = m_cols - m_cursorX;

    Row& row = m_screen[(size_t)m_cursorY];
    for (int c = m_cols - 1; c >= m_cursorX + n; --c) {
        row[(size_t)c] = row[(size_t)(c - n)];
    }
    Cell blank = MakeBlankCell();
    for (int c = m_cursorX; c < m_cursorX + n; ++c) row[(size_t)c] = blank;
}

void VtTerminal::DeleteChars(int n) {
    if (n <= 0 || m_cursorX >= m_cols) return;
    if (m_cursorX + n > m_cols) n = m_cols - m_cursorX;

    Row& row = m_screen[(size_t)m_cursorY];
    for (int c = m_cursorX; c < m_cols - n; ++c) {
        row[(size_t)c] = row[(size_t)(c + n)];
    }
    Cell blank = MakeBlankCell();
    for (int c = m_cols - n; c < m_cols; ++c) row[(size_t)c] = blank;
}

void VtTerminal::EraseChars(int n) {
    if (n <= 0) return;
    Cell blank = MakeBlankCell();
    Row& row = m_screen[(size_t)m_cursorY];
    int end = m_cursorX + n;
    if (end > m_cols) end = m_cols;
    for (int c = m_cursorX; c < end; ++c) row[(size_t)c] = blank;
}

void VtTerminal::InsertLines(int n) {
    if (n <= 0) return;
    if (m_cursorY < m_scrollTop || m_cursorY > m_scrollBottom) return;
    if (n > m_scrollBottom - m_cursorY + 1) n = m_scrollBottom - m_cursorY + 1;

    for (int i = 0; i < n; ++i) {
        for (int r = m_scrollBottom; r > m_cursorY; --r) {
            m_screen[(size_t)r] = std::move(m_screen[(size_t)r - 1]);
        }
        m_screen[(size_t)m_cursorY] = MakeBlankRow();
    }
    m_cursorX = 0;
    m_wrapPending = false;
}

void VtTerminal::DeleteLines(int n) {
    if (n <= 0) return;
    if (m_cursorY < m_scrollTop || m_cursorY > m_scrollBottom) return;
    if (n > m_scrollBottom - m_cursorY + 1) n = m_scrollBottom - m_cursorY + 1;

    for (int i = 0; i < n; ++i) {
        for (int r = m_cursorY; r < m_scrollBottom; ++r) {
            m_screen[(size_t)r] = std::move(m_screen[(size_t)r + 1]);
        }
        m_screen[(size_t)m_scrollBottom] = MakeBlankRow();
    }
    m_cursorX = 0;
    m_wrapPending = false;
}

// ---------------------------------------------------------------------------
//  SGR
// ---------------------------------------------------------------------------
static bool ParseExtendedColor(const std::vector<int>& p, size_t& i, uint32_t& out) {
    if (i + 1 >= p.size()) return false;
    int mode = p[i + 1];

    if (mode == 5) {
        if (i + 2 >= p.size()) return false;
        int idx = p[i + 2];
        if (idx < 0) idx = 0;
        if (idx < 16) {
            out = Theme::Ansi[idx];
        } else if (idx < 232) {
            int n = idx - 16;
            int r = n / 36, g = (n / 6) % 6, b = n % 6;
            auto lv = [](int v) -> uint32_t { return (v == 0) ? 0u : (uint32_t)(55 + v * 40); };
            out = (lv(r) << 16) | (lv(g) << 8) | lv(b);
        } else {
            int v = 8 + (idx - 232) * 10;
            if (v > 255) v = 255;
            out = ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
        }
        i += 2;
        return true;
    }

    if (mode == 2) {
        // 可能是 2;r;g;b，也可能是 2;;r;g;b（带色彩空间占位）
        size_t base = i + 2;
        if (base < p.size() && p[base] < 0) ++base;
        if (base + 2 >= p.size()) return false;

        int r = p[base], g = p[base + 1], b = p[base + 2];
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        i = base + 2;
        return true;
    }
    return false;
}

void VtTerminal::ApplySgr(const std::vector<int>& p) {
    if (p.empty()) { ResetAttrs(); return; }

    for (size_t i = 0; i < p.size(); ++i) {
        int v = p[i];
        if (v < 0) v = 0;

        switch (v) {
        case 0:  ResetAttrs(); break;
        case 1:  m_attr |= A_Bold; break;
        case 2:  m_attr |= A_Dim; break;
        case 3:  m_attr |= A_Italic; break;
        case 4:  m_attr |= A_Underline; break;
        case 5:  m_attr |= A_Blink; break;
        case 7:  m_attr |= A_Reverse; break;
        case 8:  m_attr |= A_Hidden; break;
        case 9:  m_attr |= A_Strike; break;
        case 21: m_attr &= (uint16_t)~A_Bold; break;
        case 22: m_attr &= (uint16_t)~(A_Bold | A_Dim); break;
        case 23: m_attr &= (uint16_t)~A_Italic; break;
        case 24: m_attr &= (uint16_t)~A_Underline; break;
        case 25: m_attr &= (uint16_t)~A_Blink; break;
        case 27: m_attr &= (uint16_t)~A_Reverse; break;
        case 28: m_attr &= (uint16_t)~A_Hidden; break;
        case 29: m_attr &= (uint16_t)~A_Strike; break;
        case 39: m_fg = Theme::ColorDefaultFg; break;
        case 49: m_bg = Theme::ColorDefaultBg; break;
        case 38:
        case 48: {
            uint32_t col = 0;
            if (ParseExtendedColor(p, i, col)) {
                if (v == 38) m_fg = col; else m_bg = col;
            }
            break;
        }
        default:
            if (v >= 30 && v <= 37)       m_fg = Theme::Ansi[v - 30];
            else if (v >= 40 && v <= 47)  m_bg = Theme::Ansi[v - 40];
            else if (v >= 90 && v <= 97)  m_fg = Theme::Ansi[v - 90 + 8];
            else if (v >= 100 && v <= 107) m_bg = Theme::Ansi[v - 100 + 8];
            break;
        }
    }
}

// ---------------------------------------------------------------------------
//  备用屏
// ---------------------------------------------------------------------------
void VtTerminal::SwitchAltScreen(bool enable) {
    if (enable == m_altActive) return;

    if (enable) {
        m_mainSaved = std::move(m_screen);
        m_screen.clear();
        InitScreen(m_screen);
        m_altActive = true;
        m_savedAlt = SavedCursor();
        m_cursorX = 0;
        m_cursorY = 0;
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
    } else {
        m_screen = std::move(m_mainSaved);
        m_mainSaved.clear();
        m_altActive = false;
        if ((int)m_screen.size() != m_rows) InitScreen(m_screen);
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
    }
    m_wrapPending = false;
}

// ---------------------------------------------------------------------------
//  C0
// ---------------------------------------------------------------------------
void VtTerminal::ExecuteC0(unsigned char c) {
    switch (c) {
    case 0x07:  // BEL
        m_bell = true;
        m_bellFlash = true;
        break;
    case 0x08: Backspace(); break;
    case 0x09: Tab(); break;
    case 0x0A: case 0x0B: case 0x0C: LineFeed(); break;
    case 0x0D: CarriageReturn(); break;
    case 0x0E: m_activeCharset = 1; break;   // SO
    case 0x0F: m_activeCharset = 0; break;   // SI
    case 0x1B: m_st = St::Escape; break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
//  ESC
// ---------------------------------------------------------------------------
void VtTerminal::HandleEsc(unsigned char c) {
    switch (c) {
    case '[':
        m_st = St::CsiEntry;
        m_params.clear();
        m_private = false;
        m_csiIntermediate = 0;
        return;
    case ']':
        m_st = St::OscString;
        m_strBuf.clear();
        m_strEsc = false;
        return;
    case 'P':
        m_st = St::DcsPassthrough;
        m_strBuf.clear();
        m_strEsc = false;
        return;
    case 'X': case '^': case '_':
        m_st = St::SosPmApc;
        m_strBuf.clear();
        m_strEsc = false;
        return;
    case '(':
    case ')':
        m_escIntermediate = (wchar_t)c;
        m_st = St::EscapeIntermediate;
        return;
    case '7': SaveCursor();    m_st = St::Ground; return;   // DECSC
    case '8': RestoreCursor(); m_st = St::Ground; return;   // DECRC
    case 'D': LineFeed();      m_st = St::Ground; return;   // IND
    case 'E': CarriageReturn(); LineFeed(); m_st = St::Ground; return;  // NEL
    case 'M': ReverseIndex();  m_st = St::Ground; return;   // RI
    case 'H': m_st = St::Ground; return;                    // HTS（不实现制表位）
    case 'Z':                                               // DECID
        m_st = St::Ground;
        return;
    case 'c':                                               // RIS
        Reset(true);
        return;
    case '=': m_appKeypad = true;  m_st = St::Ground; return;
    case '>': m_appKeypad = false; m_st = St::Ground; return;
    case 'N': case 'O': m_singleShift = true; m_st = St::Ground; return;
    default:
        m_st = St::Ground;
        return;
    }
}

// ---------------------------------------------------------------------------
//  CSI
// ---------------------------------------------------------------------------
void VtTerminal::HandleCsi(unsigned char c) {
    if (c >= '0' && c <= '9') {
        if (m_params.empty()) m_params.push_back(0);
        if (m_params.back() < 0) m_params.back() = 0;
        if (m_params.back() < 1000000) {
            m_params.back() = m_params.back() * 10 + (int)(c - '0');
        }
        m_st = St::CsiParam;
        return;
    }
    if (c == ';') {
        m_params.push_back(-1);
        m_st = St::CsiParam;
        return;
    }
    if (c == ':') {                      // 子参数分隔，按同级处理
        m_params.push_back(-1);
        m_st = St::CsiParam;
        return;
    }
    if (c == '?') {
        if (m_st == St::CsiEntry) { m_private = true; return; }
        m_st = St::CsiIgnore;
        return;
    }
    if (c == '<' || c == '=' || c == '>') {
        if (m_st == St::CsiEntry) { m_private = true; m_csiIntermediate = (wchar_t)c; return; }
        m_st = St::CsiIgnore;
        return;
    }
    if (c >= 0x20 && c <= 0x2F) {
        m_csiIntermediate = (wchar_t)c;
        m_st = St::CsiIntermediate;
        return;
    }
    if (c >= 0x40 && c <= 0x7E) {
        CsiDispatch((wchar_t)c);
        return;
    }
    if (c < 0x20) {
        ExecuteC0(c);
        return;
    }
    m_st = St::CsiIgnore;
}

void VtTerminal::CsiDispatch(wchar_t final) {
    m_st = St::Ground;
    const bool priv = m_private;
    const wchar_t inter = m_csiIntermediate;

    // ---- 私有模式：? h / ? l ----
    if (priv && (final == L'h' || final == L'l')) {
        bool set = (final == L'h');
        for (int v : m_params) {
            if (v < 0) continue;
            switch (v) {
            case 1:   m_appCursorKeys = set; break;
            case 6:   m_originMode = set; SetCursor(0, 0); break;
            case 7:   m_autoWrap = set; break;
            case 12:  m_cursorBlink = set; break;
            case 25:  m_cursorVisible = set; break;
            case 1000: case 1002: case 1003:
                m_mouseMode = set ? v : (m_mouseMode == v ? 0 : m_mouseMode);
                break;
            case 1004: break;                       // 焦点上报，忽略
            case 1005: break;                       // UTF-8 鼠标，忽略
            case 1006: m_mouseSgr = set; break;
            case 1015: break;
            case 1047: case 47:
                SwitchAltScreen(set);
                break;
            case 1048:
                if (set) SaveCursor(); else RestoreCursor();
                break;
            case 1049:
                if (set) { SaveCursor(); SwitchAltScreen(true); }
                else     { SwitchAltScreen(false); RestoreCursor(); }
                break;
            case 2004: m_bracketedPaste = set; break;
            default: break;
            }
        }
        m_params.clear();
        m_private = false;
        m_csiIntermediate = 0;
        return;
    }

    // ---- 非私有模式 ----
    if (!priv && (final == L'h' || final == L'l')) {
        bool set = (final == L'h');
        for (int v : m_params) {
            if (v == 4)  m_insertMode = set;
            if (v == 20) { /* LNM */ }
        }
        m_params.clear();
        m_csiIntermediate = 0;
        return;
    }

    // ---- 中间字符带 "!" 的软重置 ----
    if (inter == L'!' && final == L'p') {
        Reset(false);
        return;
    }

    switch (final) {
    case L'@': InsertChars(Param(0, 1)); break;
    case L'A': MoveCursor(0, -Param(0, 1)); break;
    case L'B': MoveCursor(0,  Param(0, 1)); break;
    case L'C': MoveCursor( Param(0, 1), 0); break;
    case L'D': MoveCursor(-Param(0, 1), 0); break;
    case L'E': MoveCursor(0, Param(0, 1)); CarriageReturn(); break;
    case L'F': MoveCursor(0, -Param(0, 1)); CarriageReturn(); break;
    case L'G': SetCursor(m_cursorY, Param(0, 1) - 1); break;
    case L'H':
    case L'f': SetCursor(Param(0, 1) - 1, Param(1, 1) - 1); break;
    case L'I': {                      // CHT 前进 n 个制表位
        int n = Param(0, 1);
        for (int i = 0; i < n; ++i) Tab();
        break;
    }
    case L'J': EraseInDisplay(Param(0, 0, 0)); break;
    case L'K': EraseInLine(Param(0, 0, 0)); break;
    case L'L': InsertLines(Param(0, 1)); break;
    case L'M': DeleteLines(Param(0, 1)); break;
    case L'P': DeleteChars(Param(0, 1)); break;
    case L'S': ScrollUpRegion(Param(0, 1)); break;
    case L'T': ScrollDownRegion(Param(0, 1)); break;
    case L'X': EraseChars(Param(0, 1)); break;
    case L'Z': {                      // CBT 后退 n 个制表位
        int n = Param(0, 1);
        for (int i = 0; i < n; ++i) {
            int prev = ((m_cursorX > 0 ? (m_cursorX - 1) / 8 : 0)) * 8;
            m_cursorX = prev;
            m_wrapPending = false;
        }
        break;
    }
    case L'a': MoveCursor( Param(0, 1), 0); break;
    case L'b': {                      // REP 重复上一个字符
        int n = Param(0, 1);
        int px = m_cursorX - 1;
        int py = m_cursorY;
        if (px < 0 && py > 0) { py -= 1; px = m_cols - 1; }
        if (px >= 0) {
            uint32_t ch = m_screen[(size_t)py][(size_t)px].ch;
            if (ch == 0 && px > 0) ch = m_screen[(size_t)py][(size_t)(px - 1)].ch;
            if (ch) for (int i = 0; i < n; ++i) PutChar(ch);
        }
        break;
    }
    case L'c': break;                 // DA 设备属性，忽略
    case L'd': SetCursor(Param(0, 1) - 1, m_cursorX); break;
    case L'e': MoveCursor(0, Param(0, 1)); break;
    case L'g': break;                 // TBC
    case L'm': ApplySgr(m_params); break;
    case L'n':                        // DSR
        break;
    case L'q': break;                 // DECLL
    case L'r': {                      // DECSTBM
        int top = Param(0, 1) - 1;
        int bot = Param(1, m_rows) - 1;
        if (top < 0) top = 0;
        if (bot > m_rows - 1) bot = m_rows - 1;
        if (top < bot) {
            m_scrollTop = top;
            m_scrollBottom = bot;
            SetCursor(0, 0);          // originMode 时 SetCursor 会自动加上 scrollTop
        }
        break;
    }
    case L's': SaveCursor(); break;   // SCOSC
    case L'u': RestoreCursor(); break;// SCORC
    case L't': break;                 // 窗口操作，忽略
    default: break;
    }

    m_params.clear();
    m_private = false;
    m_csiIntermediate = 0;
}

// ---------------------------------------------------------------------------
//  字符串状态（OSC / DCS / SOS-PM-APC）
// ---------------------------------------------------------------------------
void VtTerminal::HandleStringByte(unsigned char c) {
    if (m_strEsc) {
        m_strEsc = false;
        if (c == '\\') { EndString(); return; }
        // 不是 ST：把之前那个 ESC 也当内容（少见），继续处理当前字节
    }

    if (c == 0x1B) { m_strEsc = true; return; }
    if (c == 0x07 && m_st == St::OscString) { EndString(); return; }
    if (c == 0x9C) { EndString(); return; }          // C1 ST

    // 限长，防止恶意序列撑爆内存
    if (m_strBuf.size() < 64 * 1024) m_strBuf.push_back((char)c);
}

void VtTerminal::EndString() {
    if (m_st == St::OscString) OscDispatch();
    m_strBuf.clear();
    m_strEsc = false;
    m_st = St::Ground;
}

void VtTerminal::OscDispatch() {
    if (m_strBuf.empty()) return;

    size_t semi = m_strBuf.find(';');
    std::string code = (semi == std::string::npos) ? m_strBuf : m_strBuf.substr(0, semi);
    std::string rest = (semi == std::string::npos) ? std::string() : m_strBuf.substr(semi + 1);

    int n = atoi(code.c_str());
    switch (n) {
    case 0:
    case 1:
    case 2:
        m_title = Utf8ToWide(rest);
        m_titleDirty = true;
        break;
    case 52: {
        // OSC 52 ; c ; base64
        size_t s2 = rest.find(';');
        if (s2 != std::string::npos) m_oscClipboard = rest.substr(s2 + 1);
        break;
    }
    default:
        break;
    }
}

std::wstring VtTerminal::TakeOscClipboard() {
    if (m_oscClipboard.empty()) return std::wstring();
    if (m_oscClipboard == "?") { m_oscClipboard.clear(); return std::wstring(); }

    std::vector<BYTE> data;
    DWORD cb = 0;
    if (!CryptStringToBinaryA(m_oscClipboard.c_str(), (DWORD)m_oscClipboard.size(),
                              CRYPT_STRING_BASE64, nullptr, &cb, nullptr, nullptr)) {
        m_oscClipboard.clear();
        return std::wstring();
    }
    data.assign(cb, 0);
    if (!CryptStringToBinaryA(m_oscClipboard.c_str(), (DWORD)m_oscClipboard.size(),
                              CRYPT_STRING_BASE64, data.data(), &cb, nullptr, nullptr)) {
        m_oscClipboard.clear();
        return std::wstring();
    }
    m_oscClipboard.clear();
    return Utf8ToWide(std::string((char*)data.data(), data.size()));
}

// ---------------------------------------------------------------------------
//  字节流入口
// ---------------------------------------------------------------------------
void VtTerminal::ProcessCodepoint(uint32_t cp) {
    if (m_st == St::Ground) {
        uint32_t mapped = MapCharset(cp);
        m_singleShift = false;
        PutChar(mapped);
    }
    // 非地面态下的多字节字符一律忽略
}

void VtTerminal::ProcessAscii(unsigned char c) {
    switch (m_st) {
    case St::Ground:
        if (c < 0x20 || c == 0x7F) ExecuteC0(c);
        else {
            uint32_t mapped = MapCharset((uint32_t)c);
            m_singleShift = false;
            PutChar(mapped);
        }
        break;

    case St::Escape:
        HandleEsc(c);
        break;

    case St::EscapeIntermediate:
        if (c >= 0x30 && c <= 0x7E) {
            // ESC ( 0 / ESC ) 0 之类
            if (m_escIntermediate == L'(') m_charsetG[0] = (c == '0') ? 1 : 0;
            else if (m_escIntermediate == L')') m_charsetG[1] = (c == '0') ? 1 : 0;
            m_st = St::Ground;
        } else if (c >= 0x20 && c <= 0x2F) {
            m_escIntermediate = (wchar_t)c;
        } else if (c < 0x20) {
            ExecuteC0(c);
        } else {
            m_st = St::Ground;
        }
        break;

    case St::CsiEntry:
    case St::CsiParam:
        HandleCsi(c);
        break;

    case St::CsiIntermediate:
        if (c >= 0x20 && c <= 0x2F) m_csiIntermediate = (wchar_t)c;
        else if (c >= 0x40 && c <= 0x7E) CsiDispatch((wchar_t)c);
        else if (c < 0x20) ExecuteC0(c);
        else m_st = St::CsiIgnore;
        break;

    case St::CsiIgnore:
        if (c >= 0x40 && c <= 0x7E) m_st = St::Ground;
        break;

    default:
        m_st = St::Ground;
        break;
    }
}

void VtTerminal::Feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)data[i];

        // 字符串状态：整段吃原始字节（里面的 UTF-8 不能被拆开解析）
        if (m_st == St::OscString || m_st == St::DcsPassthrough || m_st == St::SosPmApc) {
            HandleStringByte(c);
            continue;
        }

        // UTF-8 组装
        if (m_utf8Need > 0) {
            if ((c & 0xC0) == 0x80) {
                m_utf8Acc = (m_utf8Acc << 6) | (uint32_t)(c & 0x3F);
                if (++m_utf8Have >= m_utf8Need) {
                    uint32_t cp = m_utf8Acc;
                    m_utf8Need = 0;
                    m_utf8Have = 0;
                    m_utf8Acc = 0;
                    ProcessCodepoint(cp);
                }
                continue;
            }
            m_utf8Need = 0;
            m_utf8Have = 0;
            m_utf8Acc = 0;
        }

        if (c < 0x80) {
            ProcessAscii(c);
        } else if ((c & 0xE0) == 0xC0) {
            m_utf8Acc = c & 0x1Fu; m_utf8Need = 1; m_utf8Have = 0;
        } else if ((c & 0xF0) == 0xE0) {
            m_utf8Acc = c & 0x0Fu; m_utf8Need = 2; m_utf8Have = 0;
        } else if ((c & 0xF8) == 0xF0) {
            m_utf8Acc = c & 0x07u; m_utf8Need = 3; m_utf8Have = 0;
        } else {
            ProcessCodepoint(0xFFFD);
        }
    }
}
