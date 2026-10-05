// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  main.cpp - 主窗口：工具条 + 会话侧边栏 + 多标签终端 + 状态栏
// ===========================================================================
#include "common.h"
#include "dialogs.h"
#include "session.h"
#include "sftppanel.h"
#include "terminal.h"
#include "theme.h"
#include "widgets.h"

#include <dwmapi.h>
#include <gdiplus.h>
#include <memory>
#include <vector>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  控件 ID
// ---------------------------------------------------------------------------
enum : int {
    IDC_BTN_NEW = 1001,
    IDC_BTN_EDIT,
    IDC_BTN_DELETE,
    IDC_BTN_CONNECT,
    IDC_BTN_DISCONNECT,
    IDC_BTN_SFTP,
    IDC_BTN_ABOUT,
    IDC_TERM_BASE = 2000,
};

// 布局常量（逻辑像素）
static constexpr int TOOLBAR_H   = 46;
static constexpr int SIDEBAR_W   = 224;
static constexpr int TABBAR_H    = 34;
static constexpr int STATUSBAR_H = 26;
static constexpr int ITEM_H      = 46;

// ---------------------------------------------------------------------------
struct TabItem {
    std::unique_ptr<TerminalView> term;
    bool closeHover = false;
};

// ---------------------------------------------------------------------------
class MainWindow {
public:
    bool Create(HINSTANCE hInst, int nCmdShow);
    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void Layout();
    void Paint(HDC hdcTarget);
    void PaintSidebar(Graphics& g);
    void PaintTabBar(Graphics& g);
    void PaintStatusBar(Graphics& g);

    void CreateButtons();
    void UpdateButtons();
    void SetStatus(const std::wstring& left, const std::wstring& right);

    int  SidebarHitTest(int x, int y) const;
    int  TabHitTest(int x, int y) const;
    bool TabCloseHitTest(int x, int y) const;
    int  TabAt(int x) const;
    int  TabWidth(int index) const;

    void NewSession();
    void EditSelected();
    void DeleteSelected();
    void ConnectSelected();
    // 启动时把勾了"自动连接"的会话逐个连上（各开一个标签）
    void AutoConnectSessions();
    void ConnectSession(const Session& s);
    void ActivateTab(int index);
    void CloseTab(int index);
    void DisconnectCurrent();
    void OpenSftpForSelected();
    void ShowAbout();
    void SaveStore();

    void OnCommand(int id);
    void OnDrawItem(const DRAWITEMSTRUCT* dis);

    TabItem* ActiveTab();
    TerminalView* ActiveTerm();

    HINSTANCE m_hInst = nullptr;
    HWND      m_hwnd  = nullptr;
    SessionStore m_store;
    int m_selSession = -1;

    std::vector<std::unique_ptr<TabItem>> m_tabs;
    int m_activeTab = -1;

    HWND m_btnNew = nullptr;
    HWND m_btnEdit = nullptr;
    HWND m_btnDelete = nullptr;
    HWND m_btnConnect = nullptr;
    HWND m_btnDisconnect = nullptr;
    HWND m_btnSftp = nullptr;
    HWND m_btnAbout = nullptr;

    RECT m_toolbarRect = {};
    RECT m_sidebarRect = {};
    RECT m_tabbarRect  = {};
    RECT m_contentRect = {};
    RECT m_statusRect  = {};

    int  m_hoverSession = -1;
    int  m_hoverTab = -1;
    bool m_hoverTabClose = false;

    std::wstring m_statusLeft;
    std::wstring m_statusRight;
};

// ===========================================================================
//  创建
// ===========================================================================
LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (MainWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->m_hwnd = hwnd;
    } else {
        self = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->WndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool MainWindow::Create(HINSTANCE hInst, int nCmdShow) {
    m_hInst = hInst;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindow::StaticWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SshGuiMainWindow";
    if (!RegisterClassExW(&wc)) return false;

    int w = S(1120), h = S(720);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - w) / 2;
    int y = (sh - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    RECT rc = { 0, 0, w, h };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    m_hwnd = CreateWindowExW(
        0, L"SshGuiMainWindow", L"SSH GUI - 图形化 SSH 终端",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, this);

    if (!m_hwnd) return false;

    ApplyDarkTitleBar(m_hwnd);
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    // 自动连接放在窗口显示之后：终端要按内容区尺寸建 ConPTY，
    // 窗口还没显示时那个尺寸是错的。
    AutoConnectSessions();
    return true;
}

void MainWindow::AutoConnectSessions() {
    int started = 0;
    for (const Session& s : m_store.items) {
        if (!s.autoConnect) continue;
        if (!s.Valid()) {
            LogLine(L"会话「%s」勾了自动连接但没有主机地址，跳过", s.name.c_str());
            continue;
        }
        ConnectSession(s);      // 每个会话各开一个标签
        ++started;
    }
    if (started > 0) {
        LogLine(L"启动时自动连接了 %d 个会话", started);
        SetStatus(L"已自动连接 " + std::to_wstring(started) + L" 个会话", L"");
    }
}

// ===========================================================================
//  消息
// ===========================================================================
LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        m_store.Load();
        CreateButtons();
        Layout();
        UpdateButtons();

        SetStatus(L"就绪", L"");
        LogLine(L"主窗口已创建，会话数=%d", (int)m_store.items.size());
        return 0;
    }

    case WM_SIZE:
        Layout();
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

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(760);
        mmi->ptMinTrackSize.y = S(480);
        return 0;
    }

    case WM_DRAWITEM: {
        OnDrawItem((const DRAWITEMSTRUCT*)lp);
        return TRUE;
    }

    case WM_COMMAND: {
        OnCommand(LOWORD(wp));
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        // 侧边栏
        int idx = SidebarHitTest(x, y);
        if (idx >= 0) {
            m_selSession = idx;
            UpdateButtons();
            InvalidateRect(m_hwnd, &m_sidebarRect, FALSE);
            SetFocus(m_hwnd);
            return 0;
        }

        // 标签栏
        if (PtInRect(&m_tabbarRect, { x, y })) {
            if (TabCloseHitTest(x, y)) {
                CloseTab(TabAt(x));
                return 0;
            }
            int t = TabAt(x);
            if (t >= 0) ActivateTab(t);
            return 0;
        }
        break;
    }

    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int idx = SidebarHitTest(x, y);
        if (idx >= 0) {
            m_selSession = idx;
            ConnectSelected();
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int idx = SidebarHitTest(x, y);
        int tab = PtInRect(&m_tabbarRect, { x, y }) ? TabAt(x) : -1;
        bool tabClose = (tab >= 0) && TabCloseHitTest(x, y);

        if (idx != m_hoverSession || tab != m_hoverTab || tabClose != m_hoverTabClose) {
            m_hoverSession = idx;
            m_hoverTab = tab;
            m_hoverTabClose = tabClose;
            InvalidateRect(m_hwnd, &m_sidebarRect, FALSE);
            InvalidateRect(m_hwnd, &m_tabbarRect, FALSE);
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = m_hwnd;
            TrackMouseEvent(&tme);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (m_hoverSession >= 0 || m_hoverTab >= 0) {
            m_hoverSession = -1;
            m_hoverTab = -1;
            m_hoverTabClose = false;
            InvalidateRect(m_hwnd, &m_sidebarRect, FALSE);
            InvalidateRect(m_hwnd, &m_tabbarRect, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(m_hwnd, &pt);
        if (PtInRect(&m_sidebarRect, pt)) {
            return 0;   // 会话少时无需滚动
        }
        break;
    }

    case WM_SETFOCUS: {
        TerminalView* t = ActiveTerm();
        if (t) t->FocusTerminal();
        return 0;
    }

    case WM_APP_TERM_TITLE: {
        InvalidateRect(m_hwnd, &m_tabbarRect, FALSE);
        return 0;
    }

    case WM_CLOSE: {
        // 有活动连接时确认
        int live = 0;
        for (auto& t : m_tabs) {
            if (t->term->Connected()) ++live;
        }
        if (live > 0) {
            std::wstring text = L"当前有 " + std::to_wstring(live) + L" 个会话仍在连接。\n\n确定要退出吗？";
            if (MessageBoxW(m_hwnd, text.c_str(), L"退出 SSH GUI",
                            MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
        }
        DestroyWindow(m_hwnd);
        return 0;
    }

    case WM_DESTROY: {
        SaveStore();
        // 先断开所有终端，避免后台线程还在往已销毁的窗口投消息
        for (auto& t : m_tabs) {
            if (t->term) t->term->Disconnect();
        }
        m_tabs.clear();
        CloseAllSftpWindows();
        PostQuitMessage(0);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

// ===========================================================================
//  布局
// ===========================================================================
void MainWindow::CreateButtons() {
    struct Def { HWND* h; int id; const wchar_t* text; };
    Def defs[] = {
        { &m_btnNew,        IDC_BTN_NEW,        L"新建连接" },
        { &m_btnEdit,       IDC_BTN_EDIT,       L"编辑" },
        { &m_btnDelete,     IDC_BTN_DELETE,     L"删除" },
        { &m_btnConnect,    IDC_BTN_CONNECT,    L"连接" },
        { &m_btnDisconnect, IDC_BTN_DISCONNECT, L"断开" },
        { &m_btnSftp,       IDC_BTN_SFTP,       L"文件传输" },
        { &m_btnAbout,      IDC_BTN_ABOUT,      L"关于" },
    };

    for (auto& d : defs) {
        *d.h = CreateWindowExW(
            0, L"BUTTON", d.text,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            0, 0, 10, 10, m_hwnd, (HMENU)(INT_PTR)d.id, m_hInst, nullptr);
        if (*d.h) TrackButtonHover(*d.h);
    }
}

void MainWindow::Layout() {
    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    int toolbarH = S(TOOLBAR_H);
    int sidebarW = S(SIDEBAR_W);
    int tabH     = m_tabs.empty() ? 0 : S(TABBAR_H);
    int statusH  = S(STATUSBAR_H);

    m_toolbarRect = { 0, 0, W, toolbarH };
    m_sidebarRect = { 0, toolbarH, sidebarW, H - statusH };
    m_tabbarRect  = { sidebarW, toolbarH, W, toolbarH + tabH };
    m_contentRect = { sidebarW, toolbarH + tabH, W, H - statusH };
    m_statusRect  = { 0, H - statusH, W, H };

    // 工具条按钮
    int bh = S(28);
    int by = (toolbarH - bh) / 2;
    int x = S(10);
    auto place = [&](HWND h, int w) {
        if (!h) return;
        MoveWindow(h, x, by, S(w), bh, TRUE);
        x += S(w) + S(6);
    };

    place(m_btnNew, 92);
    x += S(8);
    place(m_btnEdit, 62);
    place(m_btnDelete, 62);
    x += S(8);
    place(m_btnConnect, 62);
    place(m_btnDisconnect, 62);
    x += S(8);
    place(m_btnSftp, 80);
    place(m_btnAbout, 62);

    // 终端视图：只显示当前标签
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        HWND h = m_tabs[i]->term->Hwnd();
        if (!h) continue;
        if ((int)i == m_activeTab) {
            SetWindowPos(h, HWND_TOP,
                         m_contentRect.left, m_contentRect.top,
                         m_contentRect.right - m_contentRect.left,
                         m_contentRect.bottom - m_contentRect.top,
                         SWP_SHOWWINDOW);
        } else {
            ShowWindow(h, SW_HIDE);
        }
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ===========================================================================
//  绘制
// ===========================================================================
void MainWindow::PaintSidebar(Graphics& g) {
    RectF r((REAL)m_sidebarRect.left, (REAL)m_sidebarRect.top,
            (REAL)(m_sidebarRect.right - m_sidebarRect.left),
            (REAL)(m_sidebarRect.bottom - m_sidebarRect.top));

    Gfx::FillRectC(g, r, Theme::SidebarBg);

    // 标题
    Font* titleFont = Gfx::UiFont(S(11), true);
    RectF tr(r.X + (REAL)S(16), r.Y + (REAL)S(10), r.Width - (REAL)S(32), (REAL)S(18));
    Gfx::Text(g, L"会话", titleFont, Theme::TextFaint, tr, 0, 1);

    int y = m_sidebarRect.top + S(34);
    int itemH = S(ITEM_H);

    if (m_store.items.empty()) {
        Font* f = Gfx::UiFont(S(12), false);
        RectF er(r.X + (REAL)S(16), (REAL)y + (REAL)S(8), r.Width - (REAL)S(32), (REAL)S(60));
        Gfx::Text(g, L"还没有会话。\n点击左上角「新建连接」\n添加第一台服务器。",
                  f, Theme::TextFaint, er, 0, 0);
        // 底部分隔线
        Gfx::Line(g, r.GetRight() - 0.5f, r.Y, r.GetRight() - 0.5f, r.GetBottom(), Theme::Border);
        return;
    }

    Font* nameFont = Gfx::UiFont(S(13), false);
    Font* subFont  = Gfx::UiFont(S(11), false);

    for (size_t i = 0; i < m_store.items.size(); ++i) {
        if (y + itemH > m_sidebarRect.bottom) break;

        const Session& s = m_store.items[i];
        bool selected = ((int)i == m_selSession);
        bool hovered  = ((int)i == m_hoverSession);

        RectF ir(r.X + (REAL)S(6), (REAL)y, r.Width - (REAL)S(12), (REAL)(itemH - S(4)));

        if (selected) {
            Gfx::FillRound(g, ir, (REAL)S(6), 0x24405F);
            Gfx::FillRound(g, RectF(ir.X, ir.Y + 6.0f, 3.0f, ir.Height - 12.0f), 1.5f, Theme::Accent);
        } else if (hovered) {
            Gfx::FillRound(g, ir, (REAL)S(6), 0x22252C);
        }

        // 左侧小方块 + 首字符
        RectF icon(ir.X + (REAL)S(10), ir.Y + (REAL)((itemH - S(4) - S(26)) / 2), (REAL)S(26), (REAL)S(26));
        Gfx::FillRound(g, icon, (REAL)S(6), selected ? Theme::Accent : Theme::BtnBg);

        std::wstring initial = L"S";
        if (!s.name.empty()) initial = s.name.substr(0, 1);
        else if (!s.host.empty()) initial = s.host.substr(0, 1);
        Gfx::Text(g, initial, Gfx::UiFont(S(13), true), 0xFFFFFF,
                  RectF(icon.X, icon.Y, icon.Width, icon.Height), 1, 1);

        // 名称与副标题
        REAL tx = icon.GetRight() + (REAL)S(10);
        REAL tw = ir.GetRight() - tx - (REAL)S(8);
        if (tw < 20.0f) tw = 20.0f;

        Gfx::Text(g, s.DisplayName(), nameFont, Theme::Text,
                  RectF(tx, ir.Y + (REAL)S(7), tw, (REAL)S(17)), 0, 1);

        std::wstring sub = s.Target();
        if (s.port != 22) sub += L":" + std::to_wstring(s.port);
        else sub += L":22";
        if (!s.forwards.empty()) {
            sub += L"  ·" + std::to_wstring(s.forwards.size()) + L" 隧道";
        }
        Gfx::Text(g, sub, subFont, Theme::TextDim,
                  RectF(tx, ir.Y + (REAL)S(24), tw, (REAL)S(15)), 0, 1);

        y += itemH;
    }

    Gfx::Line(g, r.GetRight() - 0.5f, r.Y, r.GetRight() - 0.5f, r.GetBottom(), Theme::Border);
}

int MainWindow::TabWidth(int index) const {
    if (index < 0 || index >= (int)m_tabs.size()) return 0;
    std::wstring title = m_tabs[index]->term->CurrentTitle();
    if (title.empty()) title = m_tabs[index]->term->GetSession().DisplayName();

    Font* f = Gfx::UiFont(S(12), false);
    // 粗略估算：中文字符按 1.6 倍宽度
    int px = 0;
    for (wchar_t c : title) px += (c > 0x2E80) ? S(13) : S(7);
    px += S(58);   // 图标 + 关闭按钮 + 内边距
    (void)f;
    if (px < S(130)) px = S(130);
    if (px > S(240)) px = S(240);
    return px;
}

int MainWindow::TabAt(int x) const {
    int cx = m_tabbarRect.left;
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        int w = TabWidth((int)i);
        if (x >= cx && x < cx + w) return (int)i;
        cx += w;
    }
    return -1;
}

bool MainWindow::TabCloseHitTest(int x, int y) const {
    int t = TabAt(x);
    if (t < 0) return false;
    int cx = m_tabbarRect.left;
    for (int i = 0; i < t; ++i) cx += TabWidth(i);
    int w = TabWidth(t);
    int closeLeft = cx + w - S(26);
    return (x >= closeLeft) && (y >= m_tabbarRect.top && y < m_tabbarRect.bottom);
}

void MainWindow::PaintTabBar(Graphics& g) {
    RectF r((REAL)m_tabbarRect.left, (REAL)m_tabbarRect.top,
            (REAL)(m_tabbarRect.right - m_tabbarRect.left),
            (REAL)(m_tabbarRect.bottom - m_tabbarRect.top));
    if (r.Height <= 0.0f) return;

    Gfx::FillRectC(g, r, Theme::TabIdle);

    if (m_tabs.empty()) {
        Gfx::Line(g, r.X, r.GetBottom() - 0.5f, r.GetRight(), r.GetBottom() - 0.5f, Theme::Border);
        return;
    }

    Font* f = Gfx::UiFont(S(12), false);
    REAL cx = r.X;

    for (size_t i = 0; i < m_tabs.size(); ++i) {
        REAL w = (REAL)TabWidth((int)i);
        if (cx > r.GetRight()) break;

        bool active = ((int)i == m_activeTab);
        bool hovered = ((int)i == m_hoverTab);

        RectF tr(cx, r.Y, w, r.Height);

        uint32_t bg = active ? Theme::TabActive : (hovered ? Theme::TabHover : Theme::TabIdle);
        if (hovered && !active) bg = Theme::TabHover;
        Gfx::FillRectC(g, tr, bg);

        if (active) {
            Gfx::FillRectC(g, RectF(tr.X, tr.Y, tr.Width, 2.0f), Theme::Accent);
        }

        // 状态点
        TerminalView* term = m_tabs[i]->term.get();
        uint32_t dot = Theme::TextFaint;
        if (term->Exited())      dot = Theme::Err;
        else if (term->Connected()) dot = Theme::Ok;
        else                     dot = Theme::Warn;

        REAL dx = tr.X + (REAL)S(12);
        REAL dy = tr.Y + tr.Height / 2.0f - (REAL)S(4);
        SolidBrush dotBrush(GC(dot));
        g.FillEllipse(&dotBrush, dx, dy, (REAL)S(8), (REAL)S(8));

        // 标题
        std::wstring title = term->CurrentTitle();
        if (title.empty()) title = term->GetSession().DisplayName();

        RectF tx(dx + (REAL)S(15), tr.Y, tr.Width - (REAL)S(48), tr.Height);
        Gfx::Text(g, title, f, active ? Theme::Text : Theme::TextDim, tx, 0, 1);

        // 关闭按钮
        REAL closeX = tr.GetRight() - (REAL)S(22);
        bool ch = m_hoverTabClose && ((int)i == m_hoverTab);
        if (ch || (active && hovered)) {
            Gfx::FillRound(g, RectF(closeX - 2.0f, tr.Y + tr.Height / 2.0f - (REAL)S(9),
                                    (REAL)S(18), (REAL)S(18)),
                           (REAL)S(4), 0x3A2020);
        }
        uint32_t xc = ch ? Theme::Err : Theme::TextDim;
        REAL mx = closeX + (REAL)S(7);
        REAL my = tr.Y + tr.Height / 2.0f;
        Gfx::Line(g, mx - (REAL)S(4), my - (REAL)S(4), mx + (REAL)S(4), my + (REAL)S(4), xc, 1.4f);
        Gfx::Line(g, mx + (REAL)S(4), my - (REAL)S(4), mx - (REAL)S(4), my + (REAL)S(4), xc, 1.4f);

        Gfx::Line(g, tr.GetRight() - 0.5f, tr.Y + (REAL)S(6), tr.GetRight() - 0.5f,
                  tr.GetBottom() - (REAL)S(6), Theme::Border);

        cx += w;
    }

    Gfx::Line(g, r.X, r.GetBottom() - 0.5f, r.GetRight(), r.GetBottom() - 0.5f, Theme::Border);
}

void MainWindow::PaintStatusBar(Graphics& g) {
    RectF r((REAL)m_statusRect.left, (REAL)m_statusRect.top,
            (REAL)(m_statusRect.right - m_statusRect.left),
            (REAL)(m_statusRect.bottom - m_statusRect.top));

    Gfx::FillRectC(g, r, Theme::PanelBg);
    Gfx::Line(g, r.X, r.Y + 0.5f, r.GetRight(), r.Y + 0.5f, Theme::Border);

    Font* f = Gfx::UiFont(S(11), false);
    Gfx::Text(g, m_statusLeft, f, Theme::TextDim,
              RectF(r.X + (REAL)S(12), r.Y, r.Width / 2.0f, r.Height), 0, 1);
    Gfx::Text(g, m_statusRight, f, Theme::TextFaint,
              RectF(r.X + r.Width / 2.0f, r.Y, r.Width / 2.0f - (REAL)S(12), r.Height), 2, 1);
}

void MainWindow::Paint(HDC hdcTarget) {
    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, W, H);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    {
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        Gfx::FillRectC(g, RectF(0, 0, (REAL)W, (REAL)H), Theme::Bg);

        // 工具条
        RectF tr((REAL)m_toolbarRect.left, (REAL)m_toolbarRect.top,
                 (REAL)(m_toolbarRect.right - m_toolbarRect.left),
                 (REAL)(m_toolbarRect.bottom - m_toolbarRect.top));
        Gfx::FillRectC(g, tr, Theme::PanelBg);
        Gfx::Line(g, tr.X, tr.GetBottom() - 0.5f, tr.GetRight(), tr.GetBottom() - 0.5f, Theme::Border);

        PaintSidebar(g);
        PaintTabBar(g);
        PaintStatusBar(g);

        // 终端区背景（没有标签时）
        if (m_tabs.empty()) {
            RectF cr((REAL)m_contentRect.left, (REAL)m_contentRect.top,
                     (REAL)(m_contentRect.right - m_contentRect.left),
                     (REAL)(m_contentRect.bottom - m_contentRect.top));
            Gfx::FillRectC(g, cr, Theme::TermBg);

            Font* f = Gfx::UiFont(S(14), false);
            Gfx::Text(g, L"左侧选中一台服务器，点「连接」开始会话\n"
                         L"（也可以直接双击会话项）",
                      f, Theme::TextFaint, cr, 1, 1);
        }

        g.Flush(FlushIntentionSync);
    }

    BitBlt(hdcTarget, 0, 0, W, H, hdc, 0, 0, SRCCOPY);

    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

// ===========================================================================
//  命中测试
// ===========================================================================
int MainWindow::SidebarHitTest(int x, int y) const {
    if (!PtInRect(&m_sidebarRect, { x, y })) return -1;
    int top = m_sidebarRect.top + S(34);
    if (y < top) return -1;
    int itemH = S(ITEM_H);
    int idx = (y - top) / itemH;
    if (idx < 0 || idx >= (int)m_store.items.size()) return -1;
    return idx;
}

// ===========================================================================
//  按钮状态
// ===========================================================================
void MainWindow::UpdateButtons() {
    bool hasSel = (m_selSession >= 0 && m_selSession < (int)m_store.items.size());
    TerminalView* t = ActiveTerm();
    bool connected = t && t->Connected();

    EnableWindow(m_btnEdit, hasSel);
    EnableWindow(m_btnDelete, hasSel);
    EnableWindow(m_btnConnect, hasSel);
    EnableWindow(m_btnDisconnect, connected);
    EnableWindow(m_btnSftp, hasSel);

    InvalidateRect(m_btnEdit, nullptr, TRUE);
    InvalidateRect(m_btnDelete, nullptr, TRUE);
    InvalidateRect(m_btnConnect, nullptr, TRUE);
    InvalidateRect(m_btnDisconnect, nullptr, TRUE);
    InvalidateRect(m_btnSftp, nullptr, TRUE);
}

void MainWindow::OnDrawItem(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    BtnStyle style = BtnStyle::Normal;
    if (dis->CtlID == IDC_BTN_NEW || dis->CtlID == IDC_BTN_CONNECT) style = BtnStyle::Primary;
    else if (dis->CtlID == IDC_BTN_DELETE) style = BtnStyle::Danger;
    else if (dis->CtlID == IDC_BTN_ABOUT) style = BtnStyle::Ghost;

    DrawButton(dis, ButtonHovered(dis->hwndItem), style);
}

void MainWindow::SetStatus(const std::wstring& left, const std::wstring& right) {
    m_statusLeft = left;
    m_statusRight = right;
    InvalidateRect(m_hwnd, &m_statusRect, FALSE);
}

// ===========================================================================
//  标签与会话
// ===========================================================================
TabItem* MainWindow::ActiveTab() {
    if (m_activeTab < 0 || m_activeTab >= (int)m_tabs.size()) return nullptr;
    return m_tabs[(size_t)m_activeTab].get();
}

TerminalView* MainWindow::ActiveTerm() {
    TabItem* t = ActiveTab();
    return t ? t->term.get() : nullptr;
}

void MainWindow::ActivateTab(int index) {
    if (index < 0 || index >= (int)m_tabs.size()) return;
    m_activeTab = index;
    Layout();
    if (m_tabs[(size_t)index]->term) m_tabs[(size_t)index]->term->FocusTerminal();
    UpdateButtons();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::CloseTab(int index) {
    if (index < 0 || index >= (int)m_tabs.size()) return;

    TerminalView* term = m_tabs[(size_t)index]->term.get();
    if (term && term->Connected()) {
        std::wstring msg = L"会话「" + term->GetSession().DisplayName() + L"」仍在连接中。\n\n确定关闭这个标签吗？";
        if (MessageBoxW(m_hwnd, msg.c_str(), L"关闭会话",
                        MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    if (term) term->Disconnect();

    HWND h = term ? term->Hwnd() : nullptr;
    m_tabs.erase(m_tabs.begin() + index);
    if (h) DestroyWindow(h);

    if (m_tabs.empty()) {
        m_activeTab = -1;
    } else if (m_activeTab >= (int)m_tabs.size()) {
        m_activeTab = (int)m_tabs.size() - 1;
    } else if (m_activeTab > index) {
        --m_activeTab;
    } else if (m_activeTab == index) {
        if (m_activeTab >= (int)m_tabs.size()) m_activeTab = (int)m_tabs.size() - 1;
    }

    Layout();
    UpdateButtons();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::ConnectSession(const Session& s) {
    if (!s.Valid()) {
        MessageBoxW(m_hwnd, L"这个会话还没有填写主机地址。", L"无法连接", MB_ICONWARNING);
        return;
    }

    auto item = std::make_unique<TabItem>();
    item->term = std::make_unique<TerminalView>();

    // 新标签使用当前内容区大小
    RECT cr = m_contentRect;
    if (m_tabs.empty()) {
        // 内容区高度还没算上标签栏，先给个保守值
        cr.bottom -= S(TABBAR_H);
    }

    int id = IDC_TERM_BASE + (int)m_tabs.size();
    if (!item->term->Create(m_hwnd, id, cr)) {
        MessageBoxW(m_hwnd, L"创建终端窗口失败。", L"错误", MB_ICONERROR);
        return;
    }

    TerminalView* raw = item->term.get();
    raw->SetOnTitle([this]() { InvalidateRect(m_hwnd, &m_tabbarRect, FALSE); });
    raw->SetOnClosed([this]() { UpdateButtons(); InvalidateRect(m_hwnd, nullptr, FALSE); });
    raw->SetOnActivate([this, raw]() {
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            if (m_tabs[i]->term.get() == raw) {
                if (m_activeTab != (int)i) ActivateTab((int)i);
                break;
            }
        }
    });
    raw->SetOnStatus([this]() {
        UpdateButtons();
        TerminalView* t = ActiveTerm();
        if (t) {
            std::wstring st = t->Connected() ? L"已连接  " : (t->Exited() ? L"已断开  " : L"连接中  ");
            st += t->GetSession().DisplayName() + L"  (" + t->GetSession().Target() + L")";
            if (t->Exited()) st += L"  退出码 " + std::to_wstring(t->LastExitCode());
            SetStatus(st, std::to_wstring(t->GridCols()) + L" x " + std::to_wstring(t->GridRows()));
        }
    });

    m_tabs.push_back(std::move(item));
    m_activeTab = (int)m_tabs.size() - 1;
    Layout();

    std::wstring err;
    if (!raw->Connect(s, &err)) {
        std::wstring msg = L"连接失败：\n\n" + err;
        MessageBoxW(m_hwnd, msg.c_str(), L"SSH 连接错误", MB_ICONERROR);
        raw->SetOnStatus(nullptr);
        raw->SetOnTitle(nullptr);
        raw->SetOnClosed(nullptr);
        raw->SetOnActivate(nullptr);
        CloseTab((int)m_tabs.size() - 1);
        return;
    }

    SetStatus(L"已连接  " + s.DisplayName() + L"  (" + s.Target() + L")",
              std::to_wstring(raw->GridCols()) + L" x " + std::to_wstring(raw->GridRows()));
    raw->FocusTerminal();
    UpdateButtons();
}

void MainWindow::ConnectSelected() {
    if (m_selSession < 0 || m_selSession >= (int)m_store.items.size()) {
        MessageBoxW(m_hwnd, L"请先在左侧选择一个会话。", L"提示", MB_ICONINFORMATION);
        return;
    }
    ConnectSession(m_store.items[(size_t)m_selSession]);
}

void MainWindow::DisconnectCurrent() {
    TerminalView* t = ActiveTerm();
    if (t && t->Connected()) {
        t->Disconnect();
        SetStatus(L"已断开", L"");
        UpdateButtons();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void MainWindow::NewSession() {
    Session s;
    s.name = L"";
    if (!EditSessionDialog(m_hwnd, s, true)) return;

    if (s.name.empty()) s.name = s.Target();
    m_store.items.push_back(s);
    m_selSession = (int)m_store.items.size() - 1;
    SaveStore();

    InvalidateRect(m_hwnd, &m_sidebarRect, FALSE);
    UpdateButtons();

    if (MessageBoxW(m_hwnd, L"会话已保存。现在连接吗？", L"新建会话",
                    MB_ICONQUESTION | MB_YESNO) == IDYES) {
        ConnectSelected();
    }
}

void MainWindow::EditSelected() {
    if (m_selSession < 0 || m_selSession >= (int)m_store.items.size()) return;

    Session copy = m_store.items[(size_t)m_selSession];
    if (!EditSessionDialog(m_hwnd, copy, false)) return;

    if (copy.name.empty()) copy.name = copy.Target();
    m_store.items[(size_t)m_selSession] = copy;
    SaveStore();
    InvalidateRect(m_hwnd, &m_sidebarRect, FALSE);
}

void MainWindow::DeleteSelected() {
    if (m_selSession < 0 || m_selSession >= (int)m_store.items.size()) return;

    const Session& s = m_store.items[(size_t)m_selSession];
    std::wstring msg = L"确定删除会话「" + s.DisplayName() + L"」吗？\n\n此操作不可撤销。";
    if (MessageBoxW(m_hwnd, msg.c_str(), L"删除会话",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    m_store.items.erase(m_store.items.begin() + m_selSession);
    if (m_selSession >= (int)m_store.items.size()) m_selSession = (int)m_store.items.size() - 1;
    SaveStore();
    InvalidateRect(m_hwnd, &m_sidebarRect, FALSE);
    UpdateButtons();
}

void MainWindow::OpenSftpForSelected() {
    if (m_selSession < 0 || m_selSession >= (int)m_store.items.size()) return;
    OpenSftpWindow(m_hwnd, m_store.items[(size_t)m_selSession]);
}

void MainWindow::ShowAbout() {
    std::wstring ssh = FindSshExe();
    std::wstring msg =
        L"SSH GUI  1.0\n"
        L"图形化 SSH 终端（Win32 + ConPTY）\n\n"
        L"使用的 ssh 客户端：\n" + (ssh.empty() ? L"（未找到！）" : ssh) + L"\n\n"
        L"会话配置：\n" + SessionStore::FilePath() + L"\n\n"
        L"运行日志：\n" + LogFilePath() + L"\n\n"
        L"许可证：GNU General Public License v3.0\n"
        L"Copyright (C) 2026 Lin1848624\n"
        L"本程序不提供任何担保，完整条款见 LICENSE。";
    MessageBoxW(m_hwnd, msg.c_str(), L"关于 SSH GUI", MB_ICONINFORMATION);
}

void MainWindow::SaveStore() {
    m_store.Save();
}

void MainWindow::OnCommand(int id) {
    switch (id) {
    case IDC_BTN_NEW:        NewSession();          break;
    case IDC_BTN_EDIT:       EditSelected();        break;
    case IDC_BTN_DELETE:     DeleteSelected();      break;
    case IDC_BTN_CONNECT:    ConnectSelected();     break;
    case IDC_BTN_DISCONNECT: DisconnectCurrent();   break;
    case IDC_BTN_SFTP:       OpenSftpForSelected(); break;
    case IDC_BTN_ABOUT:      ShowAbout();           break;
    default: break;
    }
}

// ===========================================================================
//  DPI 与入口
// ===========================================================================
static void EnableDpiAwareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL(WINAPI* SetDpiCtx)(HANDLE);
        auto p = (SetDpiCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (p && p((HANDLE)-4)) return;   // PER_MONITOR_AWARE_V2
        typedef BOOL(WINAPI* SetDpiAware)(void);
        auto p2 = (SetDpiAware)GetProcAddress(u, "SetProcessDPIAware");
        if (p2) p2();
    }
}

static int InitDpi() {
    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    if (dpi <= 0) dpi = 96;
    return dpi;
}

// ---------------------------------------------------------------------------
//  askpass 助手模式
//
//  ssh 需要读密码或要用户确认时，会把 SSH_ASKPASS 指向的程序拉起来，并把
//  **提示串作为唯一参数**传过来 —— 它不会传我们的自定义开关，所以命令行里
//  看不到 --askpass。这里靠环境变量 SSH_GUI_ASKPASS 认出"自己是被当 askpass
//  调用的"。那个变量由我们启动 ssh 时放进环境块，ssh 会原样传给它的子进程。
// ---------------------------------------------------------------------------
static bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    size_t n = wcslen(needle);
    if (n == 0 || hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        if (_wcsnicmp(hay.c_str() + i, needle, n) == 0) return true;
    }
    return false;
}

static int RunAskPass() {
    // ssh 把提示串放在第一个参数里，例如 "user@host's password: "
    std::wstring prompt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1 && argv[1]) prompt = argv[1];
        LocalFree(argv);
    }

    LogLine(L"askpass 被调用；argc=%d prompt='%s'", argc, prompt.c_str());

    auto emit = [](const std::wstring& s) -> int {
        std::string u8 = WideToUtf8(s);
        u8 += "\n";
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            LogLine(L"askpass: 没有有效的 stdout 句柄");
            return 1;
        }
        DWORD wrote = 0;
        if (!WriteFile(h, u8.data(), (DWORD)u8.size(), &wrote, nullptr)) {
            LogLine(L"askpass: 写 stdout 失败 err=%lu", (unsigned long)GetLastError());
            return 1;
        }
        LogLine(L"askpass: 已写出 %lu 字节", (unsigned long)wrote);
        return 0;
    };

    // 主机密钥确认提示形如 "(yes/no/[fingerprint])? "
    if (ContainsNoCase(prompt, L"yes/no") || ContainsNoCase(prompt, L"continue connecting")) {
        wchar_t flag[8] = {};
        bool autoAccept = GetEnvironmentVariableW(L"SSH_GUI_AUTO_ACCEPT", flag, 8) > 0 &&
                          flag[0] == L'1';
        LogLine(L"askpass: 这是主机密钥确认提示，自动信任=%d", (int)autoAccept);
        if (!autoAccept) {
            // 不代答：让 ssh 报失败，用户回终端里自己确认主机指纹
            return 1;
        }
        return emit(L"yes");
    }

    wchar_t buf[1024] = {};
    DWORD n = GetEnvironmentVariableW(L"SSH_GUI_PASSWORD", buf, 1024);
    LogLine(L"askpass: 密码提示，SSH_GUI_PASSWORD 长度=%lu", (unsigned long)n);
    if (n == 0 || n >= 1024) return 1;
    return emit(buf);
}

// ---------------------------------------------------------------------------
//  伪终端启动器已废弃
//
//  曾经想让 SshGui.exe 自己兼任 "--pty-launch" 启动器（先切控制台代码页再拉 ssh），
//  但实测不行：SshGui 是 GUI 子系统程序，作为 ConPTY 的直接子进程时并不会附加到
//  伪控制台，于是它启动的 ssh 会被系统另外分配一个真实控制台窗口。
//  现在改用 cmd.exe（控制台子系统）做这一层，见 TerminalView::Connect。
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // 被 ssh 当作 askpass 拉起来时，命令行里只有提示串，所以看环境变量。
    // 这里必须先调 LogInit，否则 askpass 进程一个字都不留，出问题时完全看不见。
    wchar_t askFlag[8] = {};
    if (GetEnvironmentVariableW(L"SSH_GUI_ASKPASS", askFlag, 8) > 0) {
        LogInit();
        LogLine(L"以 askpass 模式启动 (pid=%lu)", (unsigned long)GetCurrentProcessId());
        return RunAskPass();
    }

    EnableDpiAwareness();
    g_dpi = InitDpi();

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) {
        MessageBoxW(nullptr, L"GDI+ 初始化失败。", L"启动错误", MB_ICONERROR);
        return 1;
    }

    LogInit();
    LogLine(L"启动：DPI=%d cmdline=%s", g_dpi, lpCmdLine ? lpCmdLine : L"");

    int ret = 0;
    {
        MainWindow win;
        if (!win.Create(hInst, nCmdShow)) {
            MessageBoxW(nullptr, L"创建主窗口失败。", L"启动错误", MB_ICONERROR);
            GdiplusShutdown(token);
            return 1;
        }

        MSG msg = {};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // 这里不要用 IsDialogMessage：它对非对话框窗口属于未定义用法，
            // 会把终端要收的按键（TAB、方向键等）截走。对话框自己有模态循环。
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ret = (int)msg.wParam;
    }

    LogLine(L"退出，代码 %d", ret);
    GdiplusShutdown(token);
    return ret;
}
