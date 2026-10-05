// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  dialogs.cpp - 会话编辑 / 端口转发编辑（全部自建窗口，深色主题自绘）
// ===========================================================================
#include "dialogs.h"
#include "theme.h"
#include "widgets.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <imm.h>
#include <string>
#include <vector>

#pragma comment(lib, "imm32.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  控件 ID
// ---------------------------------------------------------------------------
enum : int {
    IDC_ED_NAME = 3001,
    IDC_ED_HOST,
    IDC_ED_PORT,
    IDC_ED_USER,
    IDC_ED_KEY,
    IDC_BTN_BROWSE_KEY,
    IDC_ED_DIR,
    IDC_ED_PASS,
    IDC_CHK_SAVEPASS,
    IDC_CHK_SHOWPASS,
    IDC_LB_FWD,
    IDC_BTN_FWD_ADD,
    IDC_BTN_FWD_DEL,
    IDC_ED_EXTRA,
    IDC_CHK_COMPRESS,
    IDC_CHK_KEEPALIVE,
    IDC_CHK_VERBOSE,
    IDC_CHK_AUTOACCEPT,
    IDC_CHK_AUTOCONNECT,
    IDC_BTN_OK,
    IDC_BTN_CANCEL,
};

// 端口转发子对话框
enum : int {
    IDC_FWD_LOCAL = 3101,
    IDC_FWD_REMOTE,
    IDC_FWD_DYNAMIC,
    IDC_FWD_LISTEN_HOST,
    IDC_FWD_LISTEN_PORT,
    IDC_FWD_DEST_HOST,
    IDC_FWD_DEST_PORT,
    IDC_FWD_OK,
    IDC_FWD_CANCEL,
};

// ---------------------------------------------------------------------------
//  共用工具
// ---------------------------------------------------------------------------
static HWND MakeEdit(HWND parent, int id, const std::wstring& text, DWORD extraStyle = 0) {
    HWND h = CreateWindowExW(
        0, L"EDIT", text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle,
        0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (h) SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
    return h;
}

static HWND MakeButton(HWND parent, int id, const wchar_t* text, DWORD extra = 0) {
    HWND h = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | extra,
        0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (h) TrackButtonHover(h);
    return h;
}

// 系统控件（EDIT / LISTBOX）靠 WM_CTLCOLOR* 返回的画刷擦背景。
// 千万不能返回 NULL_BRUSH —— 那是"不填充"，旧文字会一层层叠在上面。
static HBRUSH MakeBgBrush()   { return CreateSolidBrush(Rgb(Theme::Bg)); }
static HBRUSH MakeEditBrush() { return CreateSolidBrush(Rgb(Theme::EditBg)); }

static std::wstring GetText(HWND h) {
    if (!h) return std::wstring();
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return std::wstring();
    std::wstring s((size_t)len + 1, L'\0');
    GetWindowTextW(h, &s[0], len + 1);
    s.resize((size_t)len);
    return s;
}

// 数字输入框：只允许数字
static bool IsNumeric(const std::wstring& s, int& out) {
    out = 0;
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        out = out * 10 + (int)(c - L'0');
        if (out > 1000000) return false;
    }
    return true;
}

// ===========================================================================
//  会话编辑对话框
// ===========================================================================
namespace {

struct SessionDlg {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    Session* s = nullptr;
    bool isNew = false;
    bool ok = false;
    bool savePass = false;
    bool showPass = false;
    bool compress = false;
    bool keepAlive = true;
    bool verbose = false;
    bool autoAccept = false;
    bool autoConnect = false;
    std::vector<PortForward> forwards;
    int fwdSel = -1;

    HFONT font = nullptr;
    HFONT fontSmall = nullptr;
    HBRUSH brBg = nullptr;
    HBRUSH brEdit = nullptr;

    HWND edName = nullptr, edHost = nullptr, edPort = nullptr, edUser = nullptr;
    HWND edKey = nullptr, edDir = nullptr, edPass = nullptr, edExtra = nullptr;
    HWND lbFwd = nullptr;
    HWND chkSave = nullptr, chkComp = nullptr, chkAlive = nullptr, chkVerbose = nullptr;
    HWND chkShowPass = nullptr;
    HWND chkAutoAccept = nullptr;
    HWND chkAutoConnect = nullptr;
};

constexpr const wchar_t* kSessionClass = L"SshGuiSessionDlg";

void SessionDlgLayout(SessionDlg* d) {
    RECT rc = {};
    GetClientRect(d->hwnd, &rc);
    int W = rc.right;

    int labelW = S(76);
    int x = S(16) + labelW;
    int w = W - x - S(16);
    int h = S(24);
    int y = S(14);

    auto place = [&](HWND hc, int xx, int yy, int ww, int hh) {
        if (hc) MoveWindow(hc, xx, yy, ww, hh, TRUE);
    };

    // 名称
    place(GetDlgItem(d->hwnd, 0), 0, 0, 0, 0);
    int y0 = y;
    place(d->edName, x, y0, w, h);

    y += S(38);
    int portW = S(70);
    place(d->edHost, x, y, w - portW - S(52), h);
    place(d->edPort, x + w - portW, y, portW, h);

    y += S(38);
    place(d->edUser, x, y, w, h);

    y += S(38);
    int browseW = S(64);
    place(d->edKey, x, y, w - browseW - S(6), h);
    place(GetDlgItem(d->hwnd, IDC_BTN_BROWSE_KEY), x + w - browseW, y, browseW, h);

    y += S(38);
    place(d->edDir, x, y, w, h);

    y += S(38);
    int saveW = S(96);
    int showW = S(96);
    place(d->edPass, x, y, w - saveW - showW - S(12), h);
    place(d->chkShowPass, x + w - saveW - showW - S(6), y, showW, h);
    place(d->chkSave, x + w - saveW, y, saveW, h);

    y += S(32);
    place(d->chkAutoAccept, x, y, w, h);

    y += S(28);
    place(d->chkAutoConnect, x, y, w, h);

    // 端口转发
    y += S(40);
    int btnW = S(64);
    place(d->lbFwd, x, y, w - btnW - S(8), S(92));
    place(GetDlgItem(d->hwnd, IDC_BTN_FWD_ADD), x + w - btnW, y, btnW, h);
    place(GetDlgItem(d->hwnd, IDC_BTN_FWD_DEL), x + w - btnW, y + h + S(6), btnW, h);

    y += S(104);
    place(d->edExtra, x, y, w, h);

    // 三个勾选
    y += S(40);
    int cw = S(104);
    place(d->chkComp, x, y, cw, h);
    place(d->chkAlive, x + cw + S(6), y, cw, h);
    place(d->chkVerbose, x + cw * 2 + S(12), y, cw, h);

    // 底部按钮
    int by = rc.bottom - S(46);
    int bw = S(88);
    place(GetDlgItem(d->hwnd, IDC_BTN_CANCEL), W - S(16) - bw, by, bw, S(30));
    place(GetDlgItem(d->hwnd, IDC_BTN_OK), W - S(16) - bw * 2 - S(8), by, bw, S(30));
}

void SessionDlgRefreshForwards(SessionDlg* d) {
    SendMessageW(d->lbFwd, LB_RESETCONTENT, 0, 0);
    for (const auto& f : d->forwards) {
        std::wstring line = f.Describe();
        SendMessageW(d->lbFwd, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (d->fwdSel >= 0 && d->fwdSel < (int)d->forwards.size()) {
        SendMessageW(d->lbFwd, LB_SETCURSEL, (WPARAM)d->fwdSel, 0);
    } else {
        d->fwdSel = -1;
    }
}

void SessionDlgPaint(SessionDlg* d, HDC hdcTarget) {
    RECT rc = {};
    GetClientRect(d->hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, W, H);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    {
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        Gfx::FillRectC(g, RectF(0, 0, (REAL)W, (REAL)H), Theme::Bg);

        Font* f = Gfx::UiFont(S(12), false);
        Font* fb = Gfx::UiFont(S(12), true);

        int labelW = S(76);
        int x = S(16);
        int y = S(14);
        int h = S(24);

        auto label = [&](const wchar_t* text, int yy, Font* ff) {
            Gfx::Text(g, text, ff ? ff : f, Theme::TextDim,
                      RectF((REAL)x, (REAL)yy, (REAL)labelW, (REAL)h), 0, 1);
        };

        label(L"会话名称", y, nullptr);
        y += S(38);
        label(L"主机地址", y, nullptr);
        y += S(38);
        label(L"用户名", y, nullptr);
        y += S(38);
        label(L"私钥文件", y, nullptr);
        y += S(38);
        label(L"启动目录", y, nullptr);
        y += S(38);
        label(L"密码", y, nullptr);
        y += S(38);

        // 让出下面两行勾选框（"首次连接自动信任主机密钥" + "启动程序后自动连接"）。
        // 这里和 SessionDlgLayout 是各写一遍坐标的，加/删勾选框时两边都要动。
        y += S(60);

        label(L"端口转发", y + S(6), fb);
        y += S(18);
        Gfx::Text(g, L"本地 -L / 远程 -R / 动态 -D（SOCKS5）", f, Theme::TextFaint,
                  RectF((REAL)(x + labelW), (REAL)y, (REAL)(W - x - labelW - S(80)), (REAL)S(18)), 0, 1);

        y += S(28 + 92);
        label(L"额外参数", y, nullptr);

        // 分隔线
        Gfx::Line(g, (REAL)S(16), (REAL)(H - S(58)), (REAL)(W - S(16)), (REAL)(H - S(58)),
                  Theme::Border);

        g.Flush(FlushIntentionSync);
    }

    BitBlt(hdcTarget, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

void SessionDlgCreateControls(SessionDlg* d) {
    HWND h = d->hwnd;
    d->font = Gfx::MakeUiFont(S(13), false);
    d->fontSmall = Gfx::MakeUiFont(S(11), false);
    d->brBg = MakeBgBrush();
    d->brEdit = MakeEditBrush();

    const Session& s = *d->s;

    d->edName = MakeEdit(h, IDC_ED_NAME, s.name);
    d->edHost = MakeEdit(h, IDC_ED_HOST, s.host);
    d->edPort = MakeEdit(h, IDC_ED_PORT, std::to_wstring(s.port), ES_NUMBER);
    d->edUser = MakeEdit(h, IDC_ED_USER, s.user);
    d->edKey  = MakeEdit(h, IDC_ED_KEY, s.keyPath);
    d->edDir  = MakeEdit(h, IDC_ED_DIR, s.startupDir);
    d->edPass = MakeEdit(h, IDC_ED_PASS, s.password, ES_PASSWORD);
    d->edExtra = MakeEdit(h, IDC_ED_EXTRA, s.extraArgs);

    MakeButton(h, IDC_BTN_BROWSE_KEY, L"浏览...", BS_PUSHBUTTON);

    d->chkSave = MakeButton(h, IDC_CHK_SAVEPASS, L"保存密码", BS_PUSHBUTTON);
    d->chkShowPass = MakeButton(h, IDC_CHK_SHOWPASS, L"显示密码", BS_PUSHBUTTON);

    // 密码框禁用输入法：中文/全角状态下敲进去的字符会被输入法替换成别的
    // 码位，存下来的就不是用户以为的那个密码了（真实踩过：存成了
    // 汉字偏旁+韩文字母的混合乱码，命令行手输能登、程序喂的密码登不上）。
    ImmAssociateContext(d->edPass, nullptr);
    d->chkComp = MakeButton(h, IDC_CHK_COMPRESS, L"压缩传输", BS_PUSHBUTTON);
    d->chkAlive = MakeButton(h, IDC_CHK_KEEPALIVE, L"保持连接", BS_PUSHBUTTON);
    d->chkVerbose = MakeButton(h, IDC_CHK_VERBOSE, L"详细日志", BS_PUSHBUTTON);
    d->chkAutoAccept = MakeButton(h, IDC_CHK_AUTOACCEPT,
                                  L"首次连接自动信任主机密钥（省去手动输 yes，但会失去中间人防护）",
                                  BS_PUSHBUTTON);
    d->chkAutoConnect = MakeButton(h, IDC_CHK_AUTOCONNECT,
                                   L"启动程序后自动连接这个会话",
                                   BS_PUSHBUTTON);

    d->lbFwd = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
        0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LB_FWD, nullptr, nullptr);

    MakeButton(h, IDC_BTN_FWD_ADD, L"添加", BS_PUSHBUTTON);
    MakeButton(h, IDC_BTN_FWD_DEL, L"删除", BS_PUSHBUTTON);

    MakeButton(h, IDC_BTN_OK, L"确定", BS_DEFPUSHBUTTON);
    MakeButton(h, IDC_BTN_CANCEL, L"取消", BS_PUSHBUTTON);

    // 统一字体
    EnumChildWindows(h, [](HWND c, LPARAM lp) -> BOOL {
        SessionDlg* dd = (SessionDlg*)lp;
        SendMessageW(c, WM_SETFONT, (WPARAM)dd->font, TRUE);
        return TRUE;
    }, (LPARAM)d);

    d->savePass  = s.savePassword;
    d->compress  = s.compress;
    d->keepAlive = s.keepAlive;
    d->verbose   = s.verbose;
    d->autoAccept = s.autoAcceptHostKey;
    d->autoConnect = s.autoConnect;
    d->forwards  = s.forwards;
    SessionDlgRefreshForwards(d);

    int W = S(560), H = S(608);
    RECT rc = { 0, 0, W, H };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    SetWindowPos(h, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    // 居中到父窗口
    RECT pr = {};
    GetWindowRect(d->parent, &pr);
    int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int px = pr.left + (pw - ww) / 2;
    int py = pr.top + (ph - wh) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    SetWindowPos(h, HWND_TOP, px, py, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    ApplyDarkTitleBar(h);   // 显示后再设一次：只在创建时设，DWM 有时会忽略

    SessionDlgLayout(d);
}

bool SessionDlgCollect(SessionDlg* d) {
    Session& s = *d->s;

    s.name = TrimW(GetText(d->edName));
    s.host = TrimW(GetText(d->edHost));
    s.user = TrimW(GetText(d->edUser));
    s.keyPath = TrimW(GetText(d->edKey));
    s.startupDir = TrimW(GetText(d->edDir));
    s.password = GetText(d->edPass);
    s.extraArgs = TrimW(GetText(d->edExtra));
    s.savePassword = d->savePass;
    s.compress = d->compress;
    s.keepAlive = d->keepAlive;
    s.verbose = d->verbose;
    s.autoAcceptHostKey = d->autoAccept;
    s.autoConnect = d->autoConnect;
    s.forwards = d->forwards;

    if (s.host.empty()) {
        MessageBoxW(d->hwnd, L"请填写主机地址。", L"信息不完整", MB_ICONWARNING);
        SetFocus(d->edHost);
        return false;
    }

    int port = 0;
    std::wstring ps = TrimW(GetText(d->edPort));
    if (ps.empty()) {
        s.port = 22;
    } else if (!IsNumeric(ps, port) || port <= 0 || port > 65535) {
        MessageBoxW(d->hwnd, L"端口必须是 1 - 65535 之间的整数。", L"端口无效", MB_ICONWARNING);
        SetFocus(d->edPort);
        return false;
    } else {
        s.port = port;
    }

    if (!s.savePassword) {
        s.password.clear();
    } else if (!s.password.empty()) {
        // 密码里混进非 ASCII 字符，几乎总是输入法惹的祸：中文/全角状态下
        // 敲进去的字符会被替换成别的码位，用户以为存的是原密码，实际存下来
        // 的是乱码。真实踩过：命令行手输能登录，程序喂的密码登不上，解出来
        // 是汉字偏旁 + 韩文字母 + 亚美尼亚字母的混合体。
        bool nonAscii = false;
        for (wchar_t c : s.password) {
            if (c > 127) { nonAscii = true; break; }
        }
        if (nonAscii) {
            int r = MessageBoxW(d->hwnd,
                L"这个密码里含有非 ASCII 字符（中文、全角符号等）。\n\n"
                L"如果不是有意为之，多半是输入法处在中文/全角状态造成的。\n"
                L"可以先勾选「显示密码」核对一下。\n\n"
                L"仍然保存这个密码吗？",
                L"密码含非 ASCII 字符", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            if (r != IDYES) {
                SetFocus(d->edPass);
                return false;
            }
        }
    }
    if (s.name.empty()) s.name = s.Target();
    return true;
}

LRESULT CALLBACK SessionDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SessionDlg* d = (SessionDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        d = (SessionDlg*)cs->lpCreateParams;
        d->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        return TRUE;
    }
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE:
        SessionDlgCreateControls(d);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        SessionDlgPaint(d, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, Rgb(Theme::Text));
        SetBkColor(hdc, Rgb(Theme::EditBg));
        return (LRESULT)(d->brEdit ? d->brEdit : GetStockObject(BLACK_BRUSH));
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, Rgb(Theme::Text));
        SetBkColor(hdc, Rgb(Theme::EditBg));
        return (LRESULT)(d->brEdit ? d->brEdit : GetStockObject(BLACK_BRUSH));
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (!dis) return FALSE;

        if (dis->CtlID == IDC_CHK_SAVEPASS || dis->CtlID == IDC_CHK_COMPRESS ||
            dis->CtlID == IDC_CHK_KEEPALIVE || dis->CtlID == IDC_CHK_VERBOSE ||
            dis->CtlID == IDC_CHK_AUTOACCEPT || dis->CtlID == IDC_CHK_SHOWPASS ||
            dis->CtlID == IDC_CHK_AUTOCONNECT) {

            bool checked = (dis->CtlID == IDC_CHK_SAVEPASS) ? d->savePass
                         : (dis->CtlID == IDC_CHK_COMPRESS) ? d->compress
                         : (dis->CtlID == IDC_CHK_KEEPALIVE) ? d->keepAlive
                         : (dis->CtlID == IDC_CHK_VERBOSE) ? d->verbose
                         : (dis->CtlID == IDC_CHK_SHOWPASS) ? d->showPass
                         : (dis->CtlID == IDC_CHK_AUTOCONNECT) ? d->autoConnect
                                                               : d->autoAccept;
            RECT rc = dis->rcItem;
            bool hovered = ButtonHovered(dis->hwndItem);

            Graphics g(dis->hDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

            float w = (float)(rc.right - rc.left);
            float hh = (float)(rc.bottom - rc.top);
            Gfx::FillRectC(g, RectF(0, 0, w, hh), Theme::Bg);

            float box = (float)S(16);
            float by = (hh - box) / 2.0f;
            RectF br(0.5f, by, box, box);

            if (checked) {
                Gfx::FillRound(g, br, 3.0f, Theme::Accent);
                // 勾
                Pen pen(GC(0xFFFFFF), (REAL)S(2));
                g.DrawLine(&pen, br.X + box * 0.24f, br.Y + box * 0.52f,
                                 br.X + box * 0.44f, br.Y + box * 0.72f);
                g.DrawLine(&pen, br.X + box * 0.44f, br.Y + box * 0.72f,
                                 br.X + box * 0.78f, br.Y + box * 0.28f);
            } else {
                Gfx::FillRound(g, br, 3.0f, hovered ? Theme::BtnHover : Theme::EditBg);
                Gfx::StrokeRound(g, br, 3.0f, Theme::EditBorder, 1.0f);
            }

            std::wstring text = ButtonText(dis->hwndItem);
            Gfx::Text(g, text, Gfx::UiFont(S(12), false), Theme::Text,
                      RectF(box + (REAL)S(7), 0, w - box - (REAL)S(7), hh), 0, 1);

            g.Flush(FlushIntentionSync);
            return TRUE;
        }

        BtnStyle style = BtnStyle::Normal;
        if (dis->CtlID == IDC_BTN_OK) style = BtnStyle::Primary;
        else if (dis->CtlID == IDC_BTN_CANCEL || dis->CtlID == IDC_BTN_BROWSE_KEY) style = BtnStyle::Ghost;
        else if (dis->CtlID == IDC_BTN_FWD_DEL) style = BtnStyle::Danger;

        DrawButton(dis, ButtonHovered(dis->hwndItem), style);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        switch (id) {
        case IDC_CHK_SAVEPASS:
            d->savePass = !d->savePass;
            InvalidateRect(d->chkSave, nullptr, TRUE);
            return 0;
        case IDC_CHK_COMPRESS:
            d->compress = !d->compress;
            InvalidateRect(d->chkComp, nullptr, TRUE);
            return 0;
        case IDC_CHK_KEEPALIVE:
            d->keepAlive = !d->keepAlive;
            InvalidateRect(d->chkAlive, nullptr, TRUE);
            return 0;
        case IDC_CHK_VERBOSE:
            d->verbose = !d->verbose;
            InvalidateRect(d->chkVerbose, nullptr, TRUE);
            return 0;
        case IDC_CHK_SHOWPASS:
            d->showPass = !d->showPass;
            SendMessageW(d->edPass, EM_SETPASSWORDCHAR,
                         (WPARAM)(d->showPass ? 0 : L'\x25CF'), 0);
            InvalidateRect(d->edPass, nullptr, TRUE);
            InvalidateRect(d->chkShowPass, nullptr, TRUE);
            return 0;
        case IDC_CHK_AUTOCONNECT:
            d->autoConnect = !d->autoConnect;
            InvalidateRect(d->chkAutoConnect, nullptr, TRUE);
            return 0;
        case IDC_CHK_AUTOACCEPT:
            d->autoAccept = !d->autoAccept;
            InvalidateRect(d->chkAutoAccept, nullptr, TRUE);
            return 0;

        case IDC_BTN_BROWSE_KEY: {
            wchar_t buf[MAX_PATH] = {};
            std::wstring cur = GetText(d->edKey);
            if (!cur.empty()) lstrcpynW(buf, cur.c_str(), MAX_PATH);

            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"私钥文件\0*.*\0所有文件\0*.*\0\0";
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"选择私钥文件";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(d->edKey, buf);
            }
            return 0;
        }

        case IDC_BTN_FWD_ADD: {
            PortForward nf;
            nf.listenPort = 0;
            if (ForwardDialog(hwnd, nf)) {
                d->forwards.push_back(nf);
                d->fwdSel = (int)d->forwards.size() - 1;
                SessionDlgRefreshForwards(d);
            }
            return 0;
        }

        case IDC_BTN_FWD_DEL:
            if (d->fwdSel >= 0 && d->fwdSel < (int)d->forwards.size()) {
                d->forwards.erase(d->forwards.begin() + d->fwdSel);
                if (d->fwdSel >= (int)d->forwards.size()) d->fwdSel = (int)d->forwards.size() - 1;
                SessionDlgRefreshForwards(d);
            }
            return 0;

        case IDC_LB_FWD:
            if (code == LBN_SELCHANGE) {
                d->fwdSel = (int)SendMessageW(d->lbFwd, LB_GETCURSEL, 0, 0);
            } else if (code == LBN_DBLCLK) {
                int sel = (int)SendMessageW(d->lbFwd, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)d->forwards.size()) {
                    PortForward f = d->forwards[(size_t)sel];
                    if (ForwardDialog(hwnd, f)) {
                        d->forwards[(size_t)sel] = f;
                        SessionDlgRefreshForwards(d);
                    }
                }
            }
            return 0;

        case IDC_BTN_OK:
            if (SessionDlgCollect(d)) {
                d->ok = true;
                DestroyWindow(hwnd);
            }
            return 0;

        case IDC_BTN_CANCEL:
            d->ok = false;
            DestroyWindow(hwnd);
            return 0;

        default:
            break;
        }
        break;
    }

    case WM_CLOSE:
        d->ok = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 这里不能 PostQuitMessage：模态循环是靠 IsWindow() 退出的，
        // 投一个 WM_QUIT 会把主窗口的消息循环一起干掉（程序直接退出）。
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool EditSessionDialog(HWND parent, Session& s, bool isNew) {
    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SessionDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kSessionClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    SessionDlg d;
    d.parent = parent;
    d.s = &s;
    d.isNew = isNew;

    HWND h = CreateWindowExW(
        0, kSessionClass,
        isNew ? L"新建会话" : L"编辑会话",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, S(560), S(548),
        parent, nullptr, hInst, &d);

    if (!h) return false;

    ApplyDarkTitleBar(h);

    HMENU sys = GetSystemMenu(h, FALSE);
    if (sys) EnableMenuItem(sys, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);

    ShowWindow(h, SW_SHOW);
    ApplyDarkTitleBar(h);   // 显示后再设一次：只在创建时设，DWM 有时会忽略
    UpdateWindow(h);
    SetFocus(d.edHost);

    EnableWindow(parent, FALSE);

    MSG msg = {};
    while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (d.font) { DeleteObject(d.font); d.font = nullptr; }
    if (d.fontSmall) { DeleteObject(d.fontSmall); d.fontSmall = nullptr; }
    if (d.brBg) { DeleteObject(d.brBg); d.brBg = nullptr; }
    if (d.brEdit) { DeleteObject(d.brEdit); d.brEdit = nullptr; }

    return d.ok;
}

// ===========================================================================
//  端口转发编辑对话框
// ===========================================================================
namespace {

struct FwdDlg {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    PortForward* f = nullptr;
    bool ok = false;
    int kind = 0;
    HFONT font = nullptr;
    HBRUSH brEdit = nullptr;
    HWND edListenHost = nullptr, edListenPort = nullptr, edDestHost = nullptr, edDestPort = nullptr;
    HWND lbDest = nullptr;
};

constexpr const wchar_t* kFwdClass = L"SshGuiForwardDlg";

void FwdDlgLayout(FwdDlg* d) {
    RECT rc = {};
    GetClientRect(d->hwnd, &rc);
    int W = rc.right;

    int labelW = S(84);
    int x = S(16) + labelW;
    int w = W - x - S(16);
    int h = S(24);
    int y = S(14);

    // 类型分段按钮
    int segW = S(78);
    MoveWindow(GetDlgItem(d->hwnd, IDC_FWD_LOCAL), x, y, segW, h, TRUE);
    MoveWindow(GetDlgItem(d->hwnd, IDC_FWD_REMOTE), x + segW + S(4), y, segW, h, TRUE);
    MoveWindow(GetDlgItem(d->hwnd, IDC_FWD_DYNAMIC), x + (segW + S(4)) * 2, y, segW, h, TRUE);

    y += S(38);
    MoveWindow(d->edListenHost, x, y, w / 2 - S(4), h, TRUE);
    MoveWindow(d->edListenPort, x + w / 2 + S(4), y, w / 2 - S(4), h, TRUE);

    y += S(38);
    MoveWindow(d->edDestHost, x, y, w / 2 - S(4), h, TRUE);
    MoveWindow(d->edDestPort, x + w / 2 + S(4), y, w / 2 - S(4), h, TRUE);

    int by = rc.bottom - S(46);
    int bw = S(88);
    MoveWindow(GetDlgItem(d->hwnd, IDC_FWD_CANCEL), W - S(16) - bw, by, bw, S(30), TRUE);
    MoveWindow(GetDlgItem(d->hwnd, IDC_FWD_OK), W - S(16) - bw * 2 - S(8), by, bw, S(30), TRUE);
}

void FwdDlgUpdateEnabled(FwdDlg* d) {
    bool dyn = (d->kind == PortForward::Dynamic);
    EnableWindow(d->edDestHost, !dyn);
    EnableWindow(d->edDestPort, !dyn);
    InvalidateRect(d->hwnd, nullptr, FALSE);
}

void FwdDlgPaint(FwdDlg* d, HDC hdcTarget) {
    RECT rc = {};
    GetClientRect(d->hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, W, H);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    {
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        Gfx::FillRectC(g, RectF(0, 0, (REAL)W, (REAL)H), Theme::Bg);

        Font* f = Gfx::UiFont(S(12), false);
        int labelW = S(84);
        int x = S(16);
        int h = S(24);
        int y = S(14);

        auto label = [&](const wchar_t* t, int yy) {
            Gfx::Text(g, t, f, Theme::TextDim, RectF((REAL)x, (REAL)yy, (REAL)labelW, (REAL)h), 0, 1);
        };

        label(L"转发类型", y);
        y += S(38);
        label(L"监听地址", y);
        Gfx::Text(g, L"端口", f, Theme::TextFaint,
                  RectF((REAL)(x + labelW + W / 2 - labelW - S(16)) / 2.0f + (REAL)(labelW), (REAL)y, 40.0f, (REAL)h), 0, 1);
        y += S(38);
        label(L"目标主机", y);
        y += S(38);
        (void)y;

        if (d->kind == PortForward::Dynamic) {
            Gfx::Text(g, L"动态转发只需要监听地址和端口，客户端按 SOCKS5 代理使用。",
                      f, Theme::TextFaint,
                      RectF((REAL)(S(16) + labelW), (REAL)(H - S(80)), (REAL)(W - labelW - S(40)), (REAL)S(20)), 0, 1);
        }

        Gfx::Line(g, (REAL)S(16), (REAL)(H - S(58)), (REAL)(W - S(16)), (REAL)(H - S(58)), Theme::Border);
        g.Flush(FlushIntentionSync);
    }

    BitBlt(hdcTarget, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

LRESULT CALLBACK FwdDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    FwdDlg* d = (FwdDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        d = (FwdDlg*)cs->lpCreateParams;
        d->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        return TRUE;
    }
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE: {
        d->font = Gfx::MakeUiFont(S(13), false);
        d->brEdit = MakeEditBrush();
        const PortForward& f = *d->f;

        MakeButton(hwnd, IDC_FWD_LOCAL, L"本地 -L", BS_PUSHBUTTON);
        MakeButton(hwnd, IDC_FWD_REMOTE, L"远程 -R", BS_PUSHBUTTON);
        MakeButton(hwnd, IDC_FWD_DYNAMIC, L"动态 -D", BS_PUSHBUTTON);

        std::wstring lh = f.listenHost.empty() ? L"127.0.0.1" : f.listenHost;
        std::wstring lp2 = f.listenPort > 0 ? std::to_wstring(f.listenPort) : std::wstring();

        d->edListenHost = MakeEdit(hwnd, IDC_FWD_LISTEN_HOST, lh);
        d->edListenPort = MakeEdit(hwnd, IDC_FWD_LISTEN_PORT, lp2, ES_NUMBER);
        d->edDestHost   = MakeEdit(hwnd, IDC_FWD_DEST_HOST, f.destHost);
        d->edDestPort   = MakeEdit(hwnd, IDC_FWD_DEST_PORT,
                                   f.destPort > 0 ? std::to_wstring(f.destPort) : std::wstring(),
                                   ES_NUMBER);

        MakeButton(hwnd, IDC_FWD_OK, L"确定", BS_DEFPUSHBUTTON);
        MakeButton(hwnd, IDC_FWD_CANCEL, L"取消", BS_PUSHBUTTON);

        EnumChildWindows(hwnd, [](HWND c, LPARAM lparam) -> BOOL {
            FwdDlg* dd = (FwdDlg*)lparam;
            SendMessageW(c, WM_SETFONT, (WPARAM)dd->font, TRUE);
            return TRUE;
        }, (LPARAM)d);

        d->kind = f.kind;

        int W = S(470), H = S(250);
        RECT rc = { 0, 0, W, H };
        AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
        SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER);

        RECT pr = {};
        GetWindowRect(d->parent, &pr);
        int px = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
        int py = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        SetWindowPos(hwnd, HWND_TOP, px, py, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        ApplyDarkTitleBar(hwnd);

        FwdDlgLayout(d);
        FwdDlgUpdateEnabled(d);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        FwdDlgPaint(d, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, Rgb(Theme::Text));
        SetBkColor(hdc, Rgb(Theme::EditBg));
        return (LRESULT)(d->brEdit ? d->brEdit : GetStockObject(BLACK_BRUSH));
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (!dis) return FALSE;

        int id = (int)dis->CtlID;
        if (id == IDC_FWD_LOCAL || id == IDC_FWD_REMOTE || id == IDC_FWD_DYNAMIC) {
            int segKind = (id == IDC_FWD_LOCAL) ? PortForward::Local
                        : (id == IDC_FWD_REMOTE) ? PortForward::Remote
                                                 : PortForward::Dynamic;

            RECT rc = dis->rcItem;
            bool hovered = ButtonHovered(dis->hwndItem);
            bool active = (d->kind == segKind);

            Graphics g(dis->hDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

            float w = (float)(rc.right - rc.left);
            float hh = (float)(rc.bottom - rc.top);
            Gfx::FillRectC(g, RectF(0, 0, w, hh), Theme::Bg);

            RectF r(1.0f, 1.0f, w - 2.0f, hh - 2.0f);
            Gfx::FillRound(g, r, (REAL)S(5), active ? Theme::Accent
                                                    : (hovered ? Theme::BtnHover : Theme::BtnBg));
            if (!active) Gfx::StrokeRound(g, r, (REAL)S(5), Theme::BorderLight, 1.0f);

            Gfx::Text(g, ButtonText(dis->hwndItem), Gfx::UiFont(S(12), false),
                      active ? 0xFFFFFF : Theme::Text, RectF(0, 0, w, hh), 1, 1);
            g.Flush(FlushIntentionSync);
            return TRUE;
        }

        BtnStyle style = (id == IDC_FWD_OK) ? BtnStyle::Primary : BtnStyle::Ghost;
        DrawButton(dis, ButtonHovered(dis->hwndItem), style);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_FWD_LOCAL:   d->kind = PortForward::Local;   FwdDlgUpdateEnabled(d); return 0;
        case IDC_FWD_REMOTE:  d->kind = PortForward::Remote;  FwdDlgUpdateEnabled(d); return 0;
        case IDC_FWD_DYNAMIC: d->kind = PortForward::Dynamic; FwdDlgUpdateEnabled(d); return 0;

        case IDC_FWD_OK: {
            PortForward& f = *d->f;
            f.kind = d->kind;
            f.listenHost = TrimW(GetText(d->edListenHost));
            if (f.listenHost.empty()) f.listenHost = L"127.0.0.1";

            int lp2 = 0, dp = 0;
            std::wstring lps = TrimW(GetText(d->edListenPort));
            if (!IsNumeric(lps, lp2) || lp2 <= 0 || lp2 > 65535) {
                MessageBoxW(hwnd, L"监听端口必须是 1 - 65535 之间的整数。", L"端口无效", MB_ICONWARNING);
                SetFocus(d->edListenPort);
                return 0;
            }
            f.listenPort = lp2;

            if (f.kind != PortForward::Dynamic) {
                f.destHost = TrimW(GetText(d->edDestHost));
                std::wstring dps = TrimW(GetText(d->edDestPort));
                if (f.destHost.empty() || !IsNumeric(dps, dp) || dp <= 0 || dp > 65535) {
                    MessageBoxW(hwnd, L"请填写有效的目标主机和目标端口（1 - 65535）。",
                                L"信息不完整", MB_ICONWARNING);
                    SetFocus(f.destHost.empty() ? d->edDestHost : d->edDestPort);
                    return 0;
                }
                f.destPort = dp;
            } else {
                f.destHost.clear();
                f.destPort = 0;
            }

            d->ok = true;
            DestroyWindow(hwnd);
            return 0;
        }

        case IDC_FWD_CANCEL:
            d->ok = false;
            DestroyWindow(hwnd);
            return 0;

        default:
            break;
        }
        break;
    }

    case WM_CLOSE:
        d->ok = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 这里不能 PostQuitMessage：模态循环是靠 IsWindow() 退出的，
        // 投一个 WM_QUIT 会把主窗口的消息循环一起干掉（程序直接退出）。
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool ForwardDialog(HWND parent, PortForward& f) {
    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = FwdDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kFwdClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    FwdDlg d;
    d.parent = parent;
    d.f = &f;

    HWND h = CreateWindowExW(
        0, kFwdClass, L"端口转发规则",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, S(470), S(250),
        parent, nullptr, hInst, &d);

    if (!h) return false;

    ApplyDarkTitleBar(h);
    ShowWindow(h, SW_SHOW);
    ApplyDarkTitleBar(h);
    UpdateWindow(h);

    EnableWindow(parent, FALSE);

    MSG msg = {};
    while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (d.font) { DeleteObject(d.font); d.font = nullptr; }
    if (d.brEdit) { DeleteObject(d.brEdit); d.brEdit = nullptr; }
    return d.ok;
}

// ===========================================================================
//  简易输入框（新建远程目录等一次性输入）
// ===========================================================================
namespace {

struct InputBox {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    wchar_t* buf = nullptr;
    size_t cch = 0;
    bool ok = false;
    HFONT font = nullptr;
    HBRUSH brBg = nullptr;
    HBRUSH brEdit = nullptr;
    HWND edit = nullptr;
    const wchar_t* prompt = L"";
};

constexpr const wchar_t* kInputClass = L"SshGuiInputBox";

LRESULT CALLBACK InputBoxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    InputBox* d = (InputBox*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        d = (InputBox*)cs->lpCreateParams;
        d->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        return TRUE;
    }
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE: {
        d->font = Gfx::MakeUiFont(S(13), false);
        d->brBg = MakeBgBrush();
        d->brEdit = MakeEditBrush();

        HWND st = CreateWindowExW(0, L"STATIC", d->prompt,
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                  S(16), S(14), S(320), S(20),
                                  hwnd, nullptr, nullptr, nullptr);
        SendMessageW(st, WM_SETFONT, (WPARAM)d->font, TRUE);

        d->edit = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  S(16), S(40), S(320), S(26),
                                  hwnd, (HMENU)(INT_PTR)1, nullptr, nullptr);
        SendMessageW(d->edit, WM_SETFONT, (WPARAM)d->font, TRUE);
        SendMessageW(d->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));

        HWND okBtn = CreateWindowExW(0, L"BUTTON", L"确定",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
                                     S(160), S(78), S(84), S(28),
                                     hwnd, (HMENU)(INT_PTR)2, nullptr, nullptr);
        HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"取消",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         S(252), S(78), S(84), S(28),
                                         hwnd, (HMENU)(INT_PTR)3, nullptr, nullptr);
        SendMessageW(okBtn, WM_SETFONT, (WPARAM)d->font, TRUE);
        SendMessageW(cancelBtn, WM_SETFONT, (WPARAM)d->font, TRUE);
        TrackButtonHover(okBtn);
        TrackButtonHover(cancelBtn);

        SetFocus(d->edit);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(Rgb(Theme::Bg));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        bool isEdit = ((HWND)lp == d->edit);
        SetTextColor(hdc, Rgb(Theme::Text));
        SetBkColor(hdc, Rgb(isEdit ? Theme::EditBg : Theme::Bg));
        HBRUSH br = isEdit ? d->brEdit : d->brBg;
        return (LRESULT)(br ? br : GetStockObject(BLACK_BRUSH));
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (!dis) return FALSE;
        DrawButton(dis, ButtonHovered(dis->hwndItem),
                   dis->CtlID == 2 ? BtnStyle::Primary : BtnStyle::Ghost);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 2) {
            GetWindowTextW(d->edit, d->buf, (int)d->cch);
            d->ok = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == 3) {
            d->ok = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        d->ok = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 这里不能 PostQuitMessage：模态循环是靠 IsWindow() 退出的，
        // 投一个 WM_QUIT 会把主窗口的消息循环一起干掉（程序直接退出）。
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool SimpleInputBox(HWND parent, const wchar_t* title, const wchar_t* prompt,
                    wchar_t* buf, size_t cch) {
    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = InputBoxProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kInputClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    InputBox d;
    d.parent = parent;
    d.buf = buf;
    d.cch = cch;
    d.prompt = prompt ? prompt : L"";
    if (cch > 0) buf[0] = L'\0';

    RECT rc = { 0, 0, S(360), S(122) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);

    HWND h = CreateWindowExW(
        0, kInputClass, title ? title : L"输入",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        parent, nullptr, hInst, &d);

    if (!h) return false;

    ApplyDarkTitleBar(h);

    RECT pr = {};
    GetWindowRect(parent, &pr);
    int px = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    int py = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    SetWindowPos(h, HWND_TOP, px, py, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    ApplyDarkTitleBar(h);   // 显示后再设一次：只在创建时设，DWM 有时会忽略

    EnableWindow(parent, FALSE);

    MSG msg = {};
    while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    if (d.font) { DeleteObject(d.font); d.font = nullptr; }
    if (d.brBg) { DeleteObject(d.brBg); d.brBg = nullptr; }
    if (d.brEdit) { DeleteObject(d.brEdit); d.brEdit = nullptr; }
    return d.ok;
}
