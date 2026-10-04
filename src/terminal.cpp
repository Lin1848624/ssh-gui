// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  terminal.cpp - 终端视图实现
// ===========================================================================
#include "terminal.h"
#include "theme.h"

#include <dwmapi.h>

// ---------------------------------------------------------------------------
//  局部工具
// ---------------------------------------------------------------------------
static uint32_t DimColor(uint32_t c) {
    uint32_t r = (((c >> 16) & 0xFF) * 2) / 3;
    uint32_t g = (((c >> 8) & 0xFF) * 2) / 3;
    uint32_t b = ((c & 0xFF) * 2) / 3;
    return (r << 16) | (g << 8) | b;
}

static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string WStringToUtf8(const std::wstring& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        uint32_t cp = (uint32_t)c;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
            wchar_t lo = s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)(c - 0xD800)) << 10) + (uint32_t)(lo - 0xDC00);
                ++i;
            }
        }
        AppendUtf8(out, cp);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  制表符自绘
//
//  为什么不交给字体：Consolas 里根本没有 U+2500 区的字形，GDI 靠字体链接回退到
//  SimSun；而 SimSun 的框线是 1 物理像素的细线，在 14px 字号下会被抗锯齿磨得几乎
//  看不见，终端里只剩几个角上的竖线，横线整片消失。自绘还顺带解决了另一个问题：
//  相邻格子的线能严丝合缝地接上。
// ---------------------------------------------------------------------------
namespace {

struct BoxJoints {
    int left = 0, right = 0, up = 0, down = 0;   // 0=无 1=单线 2=双线
};

bool GetBoxJoints(uint32_t cp, BoxJoints& j) {
    switch (cp) {
    // ---- 单线 ----
    case 0x2500: j = { 1, 1, 0, 0 }; return true;   // ─
    case 0x2502: j = { 0, 0, 1, 1 }; return true;   // │
    case 0x250C: j = { 0, 1, 0, 1 }; return true;   // ┌
    case 0x2510: j = { 1, 0, 0, 1 }; return true;   // ┐
    case 0x2514: j = { 0, 1, 1, 0 }; return true;   // └
    case 0x2518: j = { 1, 0, 1, 0 }; return true;   // ┘
    case 0x251C: j = { 0, 1, 1, 1 }; return true;   // ├
    case 0x2524: j = { 1, 0, 1, 1 }; return true;   // ┤
    case 0x252C: j = { 1, 1, 0, 1 }; return true;   // ┬
    case 0x2534: j = { 1, 1, 1, 0 }; return true;   // ┴
    case 0x253C: j = { 1, 1, 1, 1 }; return true;   // ┼

    // ---- 双线 ----
    case 0x2550: j = { 2, 2, 0, 0 }; return true;   // ═
    case 0x2551: j = { 0, 0, 2, 2 }; return true;   // ║
    case 0x2554: j = { 0, 2, 0, 2 }; return true;   // ╔
    case 0x2557: j = { 2, 0, 0, 2 }; return true;   // ╗
    case 0x255A: j = { 0, 2, 2, 0 }; return true;   // ╚
    case 0x255D: j = { 2, 0, 2, 0 }; return true;   // ╝
    case 0x2560: j = { 0, 2, 2, 2 }; return true;   // ╠
    case 0x2563: j = { 2, 0, 2, 2 }; return true;   // ╣
    case 0x2566: j = { 2, 2, 0, 2 }; return true;   // ╦
    case 0x2569: j = { 2, 2, 2, 0 }; return true;   // ╩
    case 0x256C: j = { 2, 2, 2, 2 }; return true;   // ╬

    // ---- 单双混合 ----
    case 0x2552: j = { 0, 1, 0, 2 }; return true;   // ╒
    case 0x2553: j = { 0, 2, 0, 1 }; return true;   // ╓
    case 0x2555: j = { 1, 0, 0, 2 }; return true;   // ╕
    case 0x2556: j = { 2, 0, 0, 1 }; return true;   // ╖
    case 0x2558: j = { 0, 1, 2, 0 }; return true;   // ╘
    case 0x2559: j = { 0, 2, 1, 0 }; return true;   // ╙
    case 0x255B: j = { 1, 0, 2, 0 }; return true;   // ╛
    case 0x255C: j = { 2, 0, 1, 0 }; return true;   // ╜
    case 0x255E: j = { 0, 1, 2, 2 }; return true;   // ╞
    case 0x255F: j = { 0, 2, 1, 1 }; return true;   // ╟
    case 0x2561: j = { 2, 0, 1, 1 }; return true;   // ╡
    case 0x2562: j = { 1, 0, 2, 2 }; return true;   // ╢
    case 0x2564: j = { 2, 2, 0, 1 }; return true;   // ╤
    case 0x2565: j = { 1, 1, 0, 2 }; return true;   // ╥
    case 0x2567: j = { 2, 2, 1, 0 }; return true;   // ╧
    case 0x2568: j = { 1, 1, 2, 0 }; return true;   // ╨
    case 0x256A: j = { 2, 2, 1, 1 }; return true;   // ╪
    case 0x256B: j = { 1, 1, 2, 2 }; return true;   // ╫

    default: return false;
    }
}

inline bool IsBoxDrawing(uint32_t cp) { return cp >= 0x2500 && cp <= 0x257F; }

void DrawBoxChar(HDC hdc, uint32_t cp, int x, int y, int cw, int ch, COLORREF col) {
    BoxJoints j;
    if (!GetBoxJoints(cp, j)) return;

    const int lw  = (S(1) > 1) ? S(1) : 1;
    const int gap = (cw >= 12) ? 2 : 1;
    const int cx  = x + cw / 2;
    const int cy  = y + ch / 2;

    HBRUSH br = CreateSolidBrush(col);

    // 单线：过格子中心
    if (j.left == 1 || j.right == 1) {
        int x0 = (j.left == 1) ? x : cx;
        int x1 = (j.right == 1) ? x + cw : cx + lw;
        RECT r = { x0, cy, x1, cy + lw };
        FillRect(hdc, &r, br);
    }
    if (j.up == 1 || j.down == 1) {
        int y0 = (j.up == 1) ? y : cy;
        int y1 = (j.down == 1) ? y + ch : cy + lw;
        RECT r = { cx, y0, cx + lw, y1 };
        FillRect(hdc, &r, br);
    }

    // 双线：中心两侧各一条
    if (j.left == 2 || j.right == 2) {
        int ys[2] = { cy - gap - lw, cy + gap };
        int x0 = (j.left == 2) ? x : cx - gap - lw;
        int x1 = (j.right == 2) ? x + cw : cx + gap + lw;
        for (int i = 0; i < 2; ++i) {
            RECT r = { x0, ys[i], x1, ys[i] + lw };
            FillRect(hdc, &r, br);
        }
    }
    if (j.up == 2 || j.down == 2) {
        int xs[2] = { cx - gap - lw, cx + gap };
        int y0 = (j.up == 2) ? y : cy - gap - lw;
        int y1 = (j.down == 2) ? y + ch : cy + gap + lw;
        for (int i = 0; i < 2; ++i) {
            RECT r = { xs[i], y0, xs[i] + lw, y1 };
            FillRect(hdc, &r, br);
        }
    }

    DeleteObject(br);
}

} // namespace

// ---------------------------------------------------------------------------
//  构造 / 析构
// ---------------------------------------------------------------------------
TerminalView::TerminalView() = default;

TerminalView::~TerminalView() {
    Disconnect();
    if (m_blinkTimer && m_hwnd) {
        KillTimer(m_hwnd, m_blinkTimer);
        m_blinkTimer = 0;
    }
    if (m_hFont)     { DeleteObject(m_hFont);     m_hFont = nullptr; }
    if (m_hFontBold) { DeleteObject(m_hFontBold); m_hFontBold = nullptr; }
}

// ---------------------------------------------------------------------------
//  窗口
// ---------------------------------------------------------------------------
LRESULT CALLBACK TerminalView::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TerminalView* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (TerminalView*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->m_hwnd = hwnd;
    } else {
        self = (TerminalView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->WndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool TerminalView::Create(HWND parent, int id, const RECT& rc) {
    m_parent = parent;
    m_id = id;

    static bool s_registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!s_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = TerminalView::StaticWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"SshGuiTerminalView";
        if (!RegisterClassExW(&wc)) {
            LogLine(L"注册终端窗口类失败 err=%lu", (unsigned long)GetLastError());
            return false;
        }
        s_registered = true;
    }

    m_hwnd = CreateWindowExW(
        0, L"SshGuiTerminalView", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        parent, (HMENU)(INT_PTR)id, hInst, this);

    return m_hwnd != nullptr;
}

// ---------------------------------------------------------------------------
//  字体与网格
// ---------------------------------------------------------------------------
void TerminalView::RebuildFont() {
    if (m_hFont)     { DeleteObject(m_hFont);     m_hFont = nullptr; }
    if (m_hFontBold) { DeleteObject(m_hFontBold); m_hFontBold = nullptr; }

    auto make = [&](int weight) -> HFONT {
        LOGFONTW lf = {};
        lf.lfHeight = -MulDiv(m_fontPt, g_dpi, 72);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        lstrcpynW(lf.lfFaceName, Theme::MonoFont, LF_FACESIZE);

        HFONT f = CreateFontIndirectW(&lf);
        if (!f) {
            lstrcpynW(lf.lfFaceName, Theme::MonoFontAlt, LF_FACESIZE);
            f = CreateFontIndirectW(&lf);
        }
        return f;
    };

    m_hFont     = make(FW_NORMAL);
    m_hFontBold = make(FW_BOLD);

    HDC hdc = GetDC(m_hwnd ? m_hwnd : nullptr);
    HGDIOBJ old = SelectObject(hdc, m_hFont);

    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    m_cellH = tm.tmHeight + S(1);

    SIZE sz = {};
    GetTextExtentPoint32W(hdc, L"M", 1, &sz);
    m_cellW = sz.cx;
    if (m_cellW < 1) m_cellW = MulDiv(m_fontPt, g_dpi, 72) / 2;

    SelectObject(hdc, old);
    ReleaseDC(m_hwnd, hdc);

    m_padX = S(5);
    m_padY = S(5);
    m_scrollbarW = S(10);
}

void TerminalView::RecalcGrid() {
    if (!m_hwnd) return;

    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    int availW = (rc.right - rc.left) - m_padX * 2 - m_scrollbarW;
    int availH = (rc.bottom - rc.top) - m_padY * 2;
    if (availW < m_cellW * 2 || availH < m_cellH * 2) return;

    int cols = availW / m_cellW;
    int rows = availH / m_cellH;
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;

    if (cols == m_gridCols && rows == m_gridRows) return;

    m_gridCols = cols;
    m_gridRows = rows;
    m_pty.Resize(cols, rows);
    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        m_vt.Resize(cols, rows);
    }
    m_viewOffset = 0;
}

// ---------------------------------------------------------------------------
//  连接
// ---------------------------------------------------------------------------
bool TerminalView::Connect(const Session& s, std::wstring* err) {
    Disconnect();

    if (!PtyProcess::Available()) {
        if (err) *err = L"当前系统不支持 ConPTY，需要 Windows 10 1809 或更高版本。";
        return false;
    }

    std::wstring ssh = FindSshExe();
    if (ssh.empty()) {
        if (err) {
            *err = L"没有找到 ssh.exe。\n\n"
                   L"请安装 Windows 的 OpenSSH 客户端：\n"
                   L"  设置 -> 系统 -> 可选功能 -> 添加功能 -> OpenSSH 客户端\n"
                   L"或把 OpenSSH 目录加入 PATH 后重启本程序。";
        }
        return false;
    }

    m_session = s;
    m_exited = false;
    m_lastExit = 0;

    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        m_vt.Reset(true);
    }
    m_viewOffset = 0;
    m_hasSelection = false;
    m_selecting = false;

    std::vector<std::wstring> sshArgs = BuildSshArgs(s, true);

    // 用 cmd.exe 包一层。
    //
    // 原因：ConPTY 的伪控制台有自己的"输出代码页"，中文系统上默认是 936(GBK)，
    // 远端发来的 UTF-8 会被整片解成"锟斤拷"。代码页是控制台对象的属性，必须在
    // **同一个**伪控制台里先切到 UTF-8 再拉起 ssh，ssh 才会继承它。
    //
    // 这层启动器不能用本程序自己：SshGui 是 GUI 子系统程序，作为 ConPTY 的直接
    // 子进程时不会附加到伪控制台，它再启动的 ssh 会被系统另开一个真实控制台窗口
    // （终端里就是一片空白 + 一个多出来的黑窗口）。cmd.exe 是控制台程序，能正确
    // 附加，所以用它当这一层。
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring cmdExe = std::wstring(sysDir) + L"\\cmd.exe";

    std::wstring rawArgs = L"/c chcp 65001 >nul & " + QuoteArg(ssh);
    for (const auto& a : sshArgs) rawArgs += L" " + QuoteArg(a);

    // 需要自动填密码时准备 askpass 环境
    std::vector<std::pair<std::wstring, std::wstring>> env;
    if (!s.password.empty()) {
        env.emplace_back(L"SSH_ASKPASS", ExePath());
        env.emplace_back(L"SSH_ASKPASS_REQUIRE", L"force");
        env.emplace_back(L"SSH_GUI_PASSWORD", s.password);
        env.emplace_back(L"DISPLAY", L":0");
    }

    m_pty.SetOnOutput([this](const char* d, size_t n) { OnPtyOutput(d, n); });
    m_pty.SetOnExit([this](DWORD code) { OnPtyExit(code); });

    std::wstring startCwd = ExeDir();
    if (!m_pty.Start(cmdExe, rawArgs, env, startCwd, m_gridCols, m_gridRows, err)) {
        if (err) {
            *err = L"启动 ssh 失败：" + *err + L"\n\n命令：" + cmdExe + L" " + rawArgs;
        }
        return false;
    }

    LogLine(L"终端已连接 %s -> %s %s", s.DisplayName().c_str(), cmdExe.c_str(), rawArgs.c_str());

    // 登录后自动 cd
    if (!s.startupDir.empty()) {
        SetTimer(m_hwnd, 2, 1200, nullptr);   // WM_TIMER id=2 时发送
    }

    UpdateCursorBlinkTimer();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (m_onStatus) m_onStatus();
    return true;
}

void TerminalView::Disconnect() {
    if (m_hwnd) KillTimer(m_hwnd, 2);
    // 先停线程再撤回调：反过来的话，读线程可能正握着旧的回调指针
    m_pty.Stop();
    m_pty.SetOnOutput(nullptr);
    m_pty.SetOnExit(nullptr);
    m_exited = true;
}

void TerminalView::OnPtyOutput(const char* data, size_t len) {
    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        m_vt.Feed(data, len);
    }

    // OSC 52 剪贴板
    std::wstring clip;
    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        clip = m_vt.TakeOscClipboard();
    }
    if (!clip.empty() && m_hwnd) {
        CopyToClipboard(m_hwnd, clip);
    }

    RequestRepaint();
}

void TerminalView::OnPtyExit(DWORD code) {
    m_lastExit = (int)code;
    m_exited = true;
    if (m_hwnd) PostMessageW(m_hwnd, WM_APP_TERM_EXIT, (WPARAM)code, 0);
}

// ---------------------------------------------------------------------------
//  重绘调度
// ---------------------------------------------------------------------------
void TerminalView::RequestRepaint() {
    if (!m_hwnd) return;
    if (m_repaintPosted.exchange(true)) return;
    PostMessageW(m_hwnd, WM_APP_TERM_DIRTY, 0, 0);
}

void TerminalView::ScrollToBottom() {
    m_viewOffset = 0;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

int TerminalView::MaxViewOffset() const {
    std::lock_guard<std::mutex> lk(m_vtMutex);
    return m_vt.ScrollbackCount();
}

void TerminalView::ScrollLines(int delta) {
    int maxOff = MaxViewOffset();
    int no = m_viewOffset + delta;
    if (no < 0) no = 0;
    if (no > maxOff) no = maxOff;
    if (no != m_viewOffset) {
        m_viewOffset = no;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
//  键盘
// ---------------------------------------------------------------------------
bool TerminalView::TranslateKey(UINT vk, bool ctrl, bool alt, bool shift, std::string& out) {
    bool app = false;
    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        app = m_vt.AppCursorKeys();
    }

    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    auto modStr = [&]() -> std::string {
        if (mod == 1) return std::string();
        char buf[16];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, ";%d", mod);
        return std::string(buf);
    };

    auto seq = [&](const char* base, char final) {
        out = "\x1b[";
        out += base;
        out += modStr();
        out += final;
    };

    switch (vk) {
    case VK_UP:     if (app && mod == 1) out = "\x1bOA"; else seq("1", 'A'); return true;
    case VK_DOWN:   if (app && mod == 1) out = "\x1bOB"; else seq("1", 'B'); return true;
    case VK_RIGHT:  if (app && mod == 1) out = "\x1bOC"; else seq("1", 'C'); return true;
    case VK_LEFT:   if (app && mod == 1) out = "\x1bOD"; else seq("1", 'D'); return true;
    case VK_HOME:   if (app && mod == 1) out = "\x1bOH"; else seq("1", 'H'); return true;
    case VK_END:    if (app && mod == 1) out = "\x1bOF"; else seq("1", 'F'); return true;

    case VK_INSERT: seq("2", '~'); return true;
    case VK_DELETE: seq("3", '~'); return true;
    case VK_PRIOR:  seq("5", '~'); return true;
    case VK_NEXT:   seq("6", '~'); return true;

    case VK_F1:  if (mod == 1) out = "\x1bOP"; else seq("1", 'P'); return true;
    case VK_F2:  if (mod == 1) out = "\x1bOQ"; else seq("1", 'Q'); return true;
    case VK_F3:  if (mod == 1) out = "\x1bOR"; else seq("1", 'R'); return true;
    case VK_F4:  if (mod == 1) out = "\x1bOS"; else seq("1", 'S'); return true;
    case VK_F5:  seq("15", '~'); return true;
    case VK_F6:  seq("17", '~'); return true;
    case VK_F7:  seq("18", '~'); return true;
    case VK_F8:  seq("19", '~'); return true;
    case VK_F9:  seq("20", '~'); return true;
    case VK_F10: seq("21", '~'); return true;
    case VK_F11: seq("23", '~'); return true;
    case VK_F12: seq("24", '~'); return true;

    case VK_TAB:
        if (shift) { out = "\x1b[Z"; return true; }
        return false;   // 普通 Tab 交给 WM_CHAR
    case VK_RETURN:
        if (alt) { out = "\x1b\r"; return true; }
        return false;
    case VK_BACK:
        if (alt) { out = "\x1b\x7f"; return true; }
        return false;
    case VK_ESCAPE:
        if (alt) { out = "\x1b\x1b"; return true; }
        return false;
    default:
        break;
    }

    // Alt + 字母/数字
    if (alt && !ctrl && vk >= 'A' && vk <= 'Z') {
        out = "\x1b";
        out += (char)tolower((int)vk);
        return true;
    }
    if (alt && !ctrl && vk >= '0' && vk <= '9') {
        out = "\x1b";
        out += (char)vk;
        return true;
    }
    return false;
}

void TerminalView::SendKeyBytes(const std::string& s) {
    SendUserInput(s);
}

// ---------------------------------------------------------------------------
//  鼠标
// ---------------------------------------------------------------------------
bool TerminalView::MouseReportingActive() const {
    std::lock_guard<std::mutex> lk(m_vtMutex);
    return m_vt.MouseMode() != 0;
}

int TerminalView::CurrentMouseMode() const {
    std::lock_guard<std::mutex> lk(m_vtMutex);
    return m_vt.MouseMode();
}

void TerminalView::SendMouseEvent(int btn, int px, int py, bool press, bool wheel, int wheelDelta) {
    int col = 0, row = 0;
    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        int c = (px - m_padX) / m_cellW;
        int r = (py - m_padY) / m_cellH;
        if (c < 0) c = 0;
        if (r < 0) r = 0;
        if (c >= m_vt.Cols()) c = m_vt.Cols() - 1;
        if (r >= m_vt.Rows()) r = m_vt.Rows() - 1;
        col = c + 1;
        row = r + 1;
    }

    std::string s = "\x1b[<";
    char buf[64];
    int code = btn;
    if (wheel) code = (wheelDelta > 0) ? 64 : 65;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d;%d;%d%c", code, col, row, press ? 'M' : 'm');
    s += buf;
    SendUserInput(s);
}

// ---------------------------------------------------------------------------
//  消息处理
// ---------------------------------------------------------------------------
LRESULT TerminalView::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        RebuildFont();
        {
            RECT rc = {};
            GetClientRect(m_hwnd, &rc);
            m_gridCols = std::max(2, (int)((rc.right - m_padX * 2 - m_scrollbarW) / m_cellW));
            m_gridRows = std::max(2, (int)((rc.bottom - m_padY * 2) / m_cellH));
            std::lock_guard<std::mutex> lk(m_vtMutex);
            m_vt.Resize(m_gridCols, m_gridRows);
        }
        return 0;

    case WM_SIZE:
        RecalcGrid();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(m_hwnd, &ps);
        Paint(hdc);
        EndPaint(m_hwnd, &ps);
        return 0;
    }

    case WM_APP_TERM_DIRTY: {
        m_repaintPosted = false;
        {
            std::lock_guard<std::mutex> lk(m_vtMutex);
            int sb = m_vt.ScrollbackCount();
            if (sb > m_lastSbCount && m_viewOffset > 0) {
                m_viewOffset += (sb - m_lastSbCount);
            }
            m_lastSbCount = sb;
            if (m_viewOffset > sb) m_viewOffset = sb;
            if (m_viewOffset < 0) m_viewOffset = 0;

            if (m_vt.TakeTitleDirty() && m_onTitle) m_onTitle();
        }
        if (m_vt.TakeBellFlash()) {
            FLASHWINFO fi = {};
            fi.cbSize = sizeof(fi);
            fi.hwnd = m_parent;
            fi.dwFlags = FLASHW_ALL;
            fi.uCount = 2;
            fi.dwTimeout = 0;
            FlashWindowEx(&fi);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP_TERM_EXIT: {
        m_exited = true;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        if (m_onClosed) m_onClosed();
        if (m_onStatus)  m_onStatus();
        return 0;
    }

    case WM_SETFOCUS:
        m_focused = true;
        UpdateCursorBlinkTimer();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;

    case WM_KILLFOCUS:
        m_focused = false;
        UpdateCursorBlinkTimer();
        m_cursorOn = true;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            m_cursorOn = !m_cursorOn;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == 2) {
            KillTimer(m_hwnd, 2);
            if (m_pty.Running() && !m_session.startupDir.empty()) {
                std::string cmd = WStringToUtf8(L"cd \"" + m_session.startupDir + L"\"\r");
                m_pty.Write(cmd.data(), cmd.size());
            }
            return 0;
        }
        break;

    case WM_LBUTTONDOWN: {
        SetFocus(m_hwnd);
        if (m_onActivate) m_onActivate();
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        if (MouseReportingActive()) {
            SendMouseEvent(0, x, y, true, false, 0);
            return 0;
        }

        int row = 0, col = 0;
        {
            std::lock_guard<std::mutex> lk(m_vtMutex);
            HitTest(x, y, row, col);
        }
        m_selecting = true;
        m_hasSelection = false;
        m_selAnchorRow = row;
        m_selAnchorCol = col;
        SetCapture(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        if (!m_selecting && CurrentMouseMode() == 1003) {
            SendMouseEvent(3, x, y, true, false, 0);
            return 0;
        }

        if (m_selecting) {
            int row = 0, col = 0;
            {
                std::lock_guard<std::mutex> lk(m_vtMutex);
                HitTest(x, y, row, col);
            }
            int r1 = m_selAnchorRow, c1 = m_selAnchorCol, r2 = row, c2 = col;
            if (r1 > r2 || (r1 == r2 && c1 > c2)) { std::swap(r1, r2); std::swap(c1, c2); }
            m_selRow1 = r1; m_selCol1 = c1;
            m_selRow2 = r2; m_selCol2 = c2;
            m_hasSelection = !(r1 == r2 && c1 == c2);
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (m_selecting) {
            m_selecting = false;
            ReleaseCapture();
            if (m_hasSelection) {
                std::wstring sel = SelectionText();
                if (!sel.empty()) CopyToClipboard(m_hwnd, sel);
            }
        }
        if (MouseReportingActive()) {
            SendMouseEvent(0, x, y, false, false, 0);
            return 0;
        }
        return 0;
    }

    case WM_MBUTTONDOWN: {
        if (MouseReportingActive()) {
            SendMouseEvent(1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, false, 0);
            return 0;
        }
        PasteClipboard();
        return 0;
    }

    case WM_RBUTTONDOWN: {
        if (MouseReportingActive()) {
            SendMouseEvent(2, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, false, 0);
            return 0;
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        if (MouseReportingActive()) {
            SendMouseEvent(2, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, false, 0);
            return 0;
        }
        // 右键：有选区则复制，否则粘贴
        if (m_hasSelection) CopyAndClearSelection();
        else PasteClipboard();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (MouseReportingActive()) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(m_hwnd, &pt);
            SendMouseEvent(0, pt.x, pt.y, true, true, delta);
            return 0;
        }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == 0) lines = 3;
        if (lines > 20) lines = 20;
        // 滚轮向前（delta > 0）是往上看历史，viewOffset 要变大。
        int step = (delta / WHEEL_DELTA) * (int)lines;
        ScrollLines(step);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        UINT vk = (UINT)wp;

        // 本程序自己的快捷键
        if (ctrl && shift) {
            if (vk == 'C') { CopyAndClearSelection(); return 0; }
            if (vk == 'V') { PasteClipboard();      return 0; }
            if (vk == 'A') { SelectAll();           return 0; }
            if (vk == VK_OEM_PLUS || vk == VK_ADD)      { ZoomFont(+1); return 0; }
            if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT){ ZoomFont(-1); return 0; }
            if (vk == '0') { ResetFontSize(); return 0; }
        }
        if (ctrl && !shift && vk == VK_HOME) { ScrollLines(+100000); return 0; }
        if (ctrl && !shift && vk == VK_END)  { ScrollToBottom();     return 0; }
        if (ctrl && !shift && vk == VK_PRIOR) { ScrollLines(+m_gridRows - 1); return 0; }
        if (ctrl && !shift && vk == VK_NEXT)  { ScrollLines(-(m_gridRows - 1)); return 0; }

        // 有选区时 Ctrl+C 复制而不是发中断
        if (ctrl && !shift && vk == 'C' && m_hasSelection) {
            CopyAndClearSelection();
            return 0;
        }

        std::string out;
        if (TranslateKey(vk, ctrl, alt, shift, out)) {
            SendKeyBytes(out);
            return 0;
        }

        // Alt+F4 / Alt+Space 要留给系统，否则窗口关不掉
        if (msg == WM_SYSKEYDOWN) {
            if (alt && (vk == VK_F4 || vk == VK_SPACE)) break;
            if (alt || vk == VK_F10) return 0;
        }
        break;
    }

    case WM_CHAR: {
        wchar_t wc = (wchar_t)wp;
        if (wc == 0) return 0;

        // 代理对组装
        if (wc >= 0xD800 && wc <= 0xDBFF) { m_highSurrogate = wc; return 0; }
        uint32_t cp = (uint32_t)wc;
        if (wc >= 0xDC00 && wc <= 0xDFFF) {
            if (m_highSurrogate) {
                cp = 0x10000 + (((uint32_t)(m_highSurrogate - 0xD800)) << 10) + (uint32_t)(wc - 0xDC00);
                m_highSurrogate = 0;
            } else {
                return 0;
            }
        } else {
            m_highSurrogate = 0;
        }

        std::string out;
        switch (cp) {
        case L'\r': out = "\r"; break;
        case L'\n': return 0;                    // 回车已由 \r 表达
        case 8:     out = "\x7f"; break;         // 退格发 DEL
        case 9:     out = "\t"; break;
        case 27:    out = "\x1b"; break;
        case 3:     // Ctrl+C
            if (m_hasSelection) { CopyAndClearSelection(); return 0; }
            out = "\x03";
            break;
        default:
            if (cp < 32) { out += (char)cp; }
            else AppendUtf8(out, cp);
            break;
        }
        SendKeyBytes(out);
        return 0;
    }

    case WM_SYSCHAR:
        // Alt+字符 到这里；Alt 已经由 WM_SYSKEYDOWN 转义过则忽略
        return 0;

    case WM_UNICHAR:
        if (wp == UNICODE_NOCHAR) return TRUE;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        }
        break;

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  颜色解析
// ---------------------------------------------------------------------------
void TerminalView::ResolveCell(const Cell& cell, bool selected,
                               uint32_t& fg, uint32_t& bg, uint16_t& attr) const {
    fg = cell.fg;
    bg = cell.bg;
    attr = cell.attr;

    if (fg == Theme::ColorDefaultFg) {
        fg = Theme::TermFg;
    } else if (attr & A_Bold) {
        for (int i = 0; i < 8; ++i) {
            if (fg == Theme::Ansi[i]) { fg = Theme::Ansi[i + 8]; break; }
        }
    }
    if (bg == Theme::ColorDefaultBg) bg = Theme::TermBg;

    if (attr & A_Reverse) { uint32_t t = fg; fg = bg; bg = t; }
    if (attr & A_Dim)     fg = DimColor(fg);
    if (attr & A_Hidden)  fg = bg;
    if (selected)         bg = Theme::SelectionBg;
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------
void TerminalView::DrawRun(HDC hdc, int absRow, int c0, int c1, int y,
                           uint32_t fg, uint32_t bg, uint16_t attr) {
    int x0 = m_padX + c0 * m_cellW;
    int x1 = m_padX + (c1 + 1) * m_cellW;
    RECT r = { x0, y, x1, y + m_cellH };

    // 组装文本与步进
    static std::wstring text;
    static std::vector<INT> dx;
    text.clear();
    dx.clear();

    bool allBlank = true;
    bool hasBox = false;
    for (int c = c0; c <= c1; ++c) {
        const Cell& cell = m_vt.CellAtAbs(absRow, c);
        if (cell.attr & A_WideTail) continue;

        int w = (cell.attr & A_Wide) ? 2 : 1;
        uint32_t ch = cell.ch ? cell.ch : (uint32_t)L' ';

        if (ch != L' ' && ch != 0) allBlank = false;

        // 制表符由 DrawBoxChar 自绘，这里只放占位空格保证后续字符位置不错位
        if (IsBoxDrawing(ch)) {
            hasBox = true;
            text.push_back(L' ');
            dx.push_back(m_cellW * w);
            continue;
        }

        if (ch >= 0x10000) {
            uint32_t v = ch - 0x10000;
            text.push_back((wchar_t)(0xD800 + (v >> 10)));
            text.push_back((wchar_t)(0xDC00 + (v & 0x3FF)));
            dx.push_back(m_cellW * w);
            dx.push_back(0);
        } else {
            text.push_back((wchar_t)ch);
            dx.push_back(m_cellW * w);
        }
    }

    HFONT font = (attr & A_Bold) ? m_hFontBold : m_hFont;
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, Rgb(bg));
    SetTextColor(hdc, Rgb(fg));

    if (allBlank || text.empty()) {
        HBRUSH br = CreateSolidBrush(Rgb(bg));
        FillRect(hdc, &r, br);
        DeleteObject(br);
    } else {
        ExtTextOutW(hdc, x0, y, ETO_OPAQUE | ETO_CLIPPED, &r,
                    text.c_str(), (UINT)text.size(), dx.data());
    }

    // 制表符自绘（必须画在 ETO_OPAQUE 之后，否则会被背景刷掉）
    if (hasBox) {
        for (int c = c0; c <= c1; ++c) {
            const Cell& cell = m_vt.CellAtAbs(absRow, c);
            if (cell.attr & A_WideTail) continue;
            uint32_t ch = cell.ch;
            if (!IsBoxDrawing(ch)) continue;
            int w = (cell.attr & A_Wide) ? 2 : 1;
            DrawBoxChar(hdc, ch, m_padX + c * m_cellW, y, m_cellW * w, m_cellH, Rgb(fg));
        }
    }

    // 下划线 / 删除线
    if (attr & (A_Underline | A_Strike)) {
        for (int c = c0; c <= c1; ++c) {
            const Cell& cell = m_vt.CellAtAbs(absRow, c);
            if (cell.attr & A_WideTail) continue;
            int w = (cell.attr & A_Wide) ? 2 : 1;
            int cx = m_padX + c * m_cellW;
            HBRUSH br = CreateSolidBrush(Rgb(fg));
            if (cell.attr & A_Underline) {
                RECT ur = { cx, y + m_cellH - S(2), cx + m_cellW * w, y + m_cellH - S(1) };
                FillRect(hdc, &ur, br);
            }
            if (cell.attr & A_Strike) {
                RECT sr = { cx, y + m_cellH / 2, cx + m_cellW * w, y + m_cellH / 2 + S(1) };
                FillRect(hdc, &sr, br);
            }
            DeleteObject(br);
        }
    }

    SelectObject(hdc, oldFont);
}

void TerminalView::DrawRow(HDC hdc, int absRow, int y, int cols) {
    int c = 0;
    while (c < cols) {
        const Cell& cell = m_vt.CellAtAbs(absRow, c);
        bool sel = IsSelected(absRow, c);

        uint32_t fg = 0, bg = 0;
        uint16_t attr = 0;
        ResolveCell(cell, sel, fg, bg, attr);

        int end = c + 1;
        while (end < cols) {
            const Cell& n = m_vt.CellAtAbs(absRow, end);
            bool nsel = IsSelected(absRow, end);
            uint32_t nfg = 0, nbg = 0;
            uint16_t nattr = 0;
            ResolveCell(n, nsel, nfg, nbg, nattr);
            if (nfg != fg || nbg != bg ||
                (nattr & ~(A_Wide | A_WideTail)) != (attr & ~(A_Wide | A_WideTail))) {
                break;
            }
            ++end;
        }

        DrawRun(hdc, absRow, c, end - 1, y, fg, bg, attr);
        c = end;
    }
}

void TerminalView::DrawScrollbar(HDC hdc, int W, int H) {
    if (m_vt.AltScreen()) return;      // 备用屏没有回看历史

    int sb = m_vt.ScrollbackCount();
    int rows = m_vt.Rows();
    if (sb <= 0 || rows <= 0) return;

    int total = sb + rows;
    int trackTop = m_padY;
    int trackH = H - m_padY * 2;
    if (trackH < 20) return;

    int thumbH = trackH * rows / total;
    if (thumbH < S(24)) thumbH = S(24);
    if (thumbH > trackH) thumbH = trackH;

    int range = trackH - thumbH;
    int pos = (sb > 0) ? (int)((long long)range * (sb - m_viewOffset) / sb) : range;

    int x = W - m_scrollbarW + S(2);
    int sw = S(6);

    HBRUSH track = CreateSolidBrush(Rgb(Theme::TermBg));
    RECT tr = { x, trackTop, x + sw, trackTop + trackH };
    FillRect(hdc, &tr, track);
    DeleteObject(track);

    HBRUSH thumb = CreateSolidBrush(Rgb(m_viewOffset > 0 ? Theme::BorderLight : Theme::Border));
    RECT th = { x, trackTop + pos, x + sw, trackTop + pos + thumbH };
    FillRect(hdc, &th, thumb);
    DeleteObject(thumb);
}

void TerminalView::DrawCursor(HDC hdc) {
    if (!m_vt.CursorVisible() || m_viewOffset != 0 || m_exited) return;

    int cx = m_padX + m_vt.CursorX() * m_cellW;
    int cy = m_padY + m_vt.CursorY() * m_cellH;

    const Cell& cell = m_vt.At(m_vt.CursorY(), m_vt.CursorX());
    uint32_t fg = 0, bg = 0;
    uint16_t attr = 0;
    ResolveCell(cell, false, fg, bg, attr);

    RECT r = { cx, cy, cx + m_cellW, cy + m_cellH };

    if (!m_focused || !m_cursorOn) {
        // 失焦或熄灭时画空心框
        HBRUSH br = CreateSolidBrush(Rgb(Theme::TextDim));
        FrameRect(hdc, &r, br);
        DeleteObject(br);
        return;
    }

    HBRUSH br = CreateSolidBrush(Rgb(Theme::TermFg));
    FillRect(hdc, &r, br);
    DeleteObject(br);

    uint32_t ch = cell.ch ? cell.ch : (uint32_t)L' ';
    if (ch != L' ') {
        std::wstring t;
        if (ch >= 0x10000) {
            uint32_t v = ch - 0x10000;
            t.push_back((wchar_t)(0xD800 + (v >> 10)));
            t.push_back((wchar_t)(0xDC00 + (v & 0x3FF)));
        } else {
            t.push_back((wchar_t)ch);
        }
        int w = (cell.attr & A_Wide) ? 2 : 1;
        INT dx[2] = { m_cellW * w, 0 };

        HGDIOBJ oldFont = SelectObject(hdc, m_hFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, Rgb(Theme::TermBg));
        ExtTextOutW(hdc, cx, cy, ETO_CLIPPED, &r, t.c_str(), (UINT)t.size(), dx);
        SelectObject(hdc, oldFont);
    }
}

void TerminalView::Paint(HDC hdcTarget) {
    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, W, H);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    std::lock_guard<std::mutex> lk(m_vtMutex);

    // 底色
    HBRUSH bgBrush = CreateSolidBrush(Rgb(Theme::TermBg));
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    int cols = m_vt.Cols();
    int rows = m_vt.Rows();
    int sb = m_vt.ScrollbackCount();
    int total = sb + rows;
    int topAbs = sb - m_viewOffset;

    for (int v = 0; v < rows; ++v) {
        int absRow = topAbs + v;
        if (absRow < 0 || absRow >= total) continue;
        int y = m_padY + v * m_cellH;
        DrawRow(hdc, absRow, y, cols);
    }

    DrawScrollbar(hdc, W, H);
    DrawCursor(hdc);

    // 已断开提示：优先画在网格下方，放不下就贴着底部画一条横幅
    if (m_exited) {
        int bh = m_cellH + S(4);
        int y = m_padY + rows * m_cellH + S(2);
        if (y + bh > H) y = H - bh;
        if (y < 0) y = 0;

        RECT br = { 0, y, W, y + bh };
        HBRUSH bb = CreateSolidBrush(Rgb(0x3A1F22));
        FillRect(hdc, &br, bb);
        DeleteObject(bb);

        std::wstring msg = L"  会话已结束，退出码 " + std::to_wstring(m_lastExit.load());
        HGDIOBJ oldFont = SelectObject(hdc, m_hFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, Rgb(0xF0A0A0));
        TextOutW(hdc, m_padX, y + S(2), msg.c_str(), (int)msg.size());
        SelectObject(hdc, oldFont);
    }

    BitBlt(hdcTarget, 0, 0, W, H, hdc, 0, 0, SRCCOPY);

    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

// ---------------------------------------------------------------------------
//  选区
// ---------------------------------------------------------------------------
bool TerminalView::IsSelected(int absRow, int col) const {
    if (!m_hasSelection) return false;
    if (absRow < m_selRow1 || absRow > m_selRow2) return false;
    if (absRow == m_selRow1 && col < m_selCol1) return false;
    if (absRow == m_selRow2 && col > m_selCol2) return false;
    return true;
}

void TerminalView::HitTest(int px, int py, int& absRow, int& col) const {
    int c = (px - m_padX) / m_cellW;
    int r = (py - m_padY) / m_cellH;
    if (c < 0) c = 0;
    if (r < 0) r = 0;
    if (c >= m_vt.Cols()) c = m_vt.Cols() - 1;
    if (r >= m_vt.Rows()) r = m_vt.Rows() - 1;

    int sb = m_vt.ScrollbackCount();
    int a = sb - m_viewOffset + r;
    if (a < 0) a = 0;
    if (a >= sb + m_vt.Rows()) a = sb + m_vt.Rows() - 1;

    col = c;
    absRow = a;
}

std::wstring TerminalView::SelectionText() const {
    std::lock_guard<std::mutex> lk(m_vtMutex);
    if (!m_hasSelection) return std::wstring();
    return m_vt.RangeText(m_selRow1, m_selCol1, m_selRow2, m_selCol2);
}

void TerminalView::CopySelection() {
    if (!m_hasSelection) return;
    std::wstring sel = SelectionText();
    if (!sel.empty()) CopyToClipboard(m_hwnd, sel);
}

void TerminalView::CopyAndClearSelection() {
    CopySelection();
    ClearSelection();
}

void TerminalView::ClearSelection() {
    m_hasSelection = false;
    m_selecting = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TerminalView::SelectAll() {
    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        int sb = m_vt.ScrollbackCount();
        m_selRow1 = 0;
        m_selCol1 = 0;
        m_selRow2 = sb + m_vt.Rows() - 1;
        m_selCol2 = m_vt.Cols() - 1;
    }
    m_hasSelection = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  输入
// ---------------------------------------------------------------------------
void TerminalView::SendUserInput(const std::string& bytes) {
    if (bytes.empty()) return;
    ScrollToBottom();
    if (m_pty.Running()) {
        m_pty.Write(bytes.data(), bytes.size());
    }
    m_cursorOn = true;
    m_lastInputTick = GetTickCount64();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TerminalView::SendRaw(const std::string& bytes) {
    if (bytes.empty()) return;
    if (m_pty.Running()) m_pty.Write(bytes.data(), bytes.size());
}

void TerminalView::PasteClipboard() {
    std::wstring text = PasteFromClipboard(m_hwnd);
    if (text.empty()) return;

    std::string payload = WStringToUtf8(text);

    bool bracketed = false;
    {
        std::lock_guard<std::mutex> lk(m_vtMutex);
        bracketed = m_vt.BracketedPaste();
    }

    if (bracketed) {
        std::string wrapped = "\x1b[200~" + payload + "\x1b[201~";
        SendUserInput(wrapped);
    } else {
        SendUserInput(payload);
    }
}

void TerminalView::FocusTerminal() {
    if (m_hwnd) SetFocus(m_hwnd);
}

// ---------------------------------------------------------------------------
//  字体缩放
// ---------------------------------------------------------------------------
void TerminalView::ZoomFont(int delta) {
    int np = m_fontPt + delta;
    if (np < 6) np = 6;
    if (np > 40) np = 40;
    if (np == m_fontPt) return;
    m_fontPt = np;
    RebuildFont();
    RecalcGrid();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TerminalView::ResetFontSize() {
    if (m_fontPt == m_baseFontPt) return;
    m_fontPt = m_baseFontPt;
    RebuildFont();
    RecalcGrid();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  光标闪烁
// ---------------------------------------------------------------------------
void TerminalView::UpdateCursorBlinkTimer() {
    if (!m_hwnd) return;
    bool want = m_focused;
    if (want && !m_blinkTimer) {
        m_blinkTimer = SetTimer(m_hwnd, 1, 530, nullptr);
    } else if (!want && m_blinkTimer) {
        KillTimer(m_hwnd, 1);
        m_blinkTimer = 0;
    }
}

std::wstring TerminalView::CurrentTitle() const {
    std::lock_guard<std::mutex> lk(m_vtMutex);
    std::wstring t = m_vt.Title();
    // ConPTY 会把子进程的 exe 路径当成初始控制台标题推过来，那不是真标题
    if (t.find(L".exe") != std::wstring::npos ||
        t.find(L":\\") != std::wstring::npos) {
        return std::wstring();
    }
    return t;
}
