// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  sftppanel.cpp - SFTP 文件传输窗口
//  远程侧通过 sftp.exe 的批处理模式（-b -）实现，每次操作起一个连接。
// ===========================================================================
#include "sftppanel.h"
#include "theme.h"
#include "widgets.h"

#include <gdiplus.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace Gdiplus;

#define WM_APP_SFTP_RESULT (WM_APP + 40)

// ---------------------------------------------------------------------------
//  控件 ID
// ---------------------------------------------------------------------------
enum : int {
    IDC_SFTP_LOCAL_LIST = 4001,
    IDC_SFTP_REMOTE_LIST,
    IDC_SFTP_UP,
    IDC_SFTP_DOWN,
    IDC_SFTP_MKDIR,
    IDC_SFTP_DELETE,
    IDC_SFTP_REFRESH,
    IDC_SFTP_CLOSE,
    IDC_SFTP_OPEN_LOCAL,
};

// ---------------------------------------------------------------------------
//  sftp 批处理执行
// ---------------------------------------------------------------------------
static std::string EscapeSftpPath(const std::string& p) {
    std::string out = "\"";
    for (char c : p) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

static std::string ToRemotePath(const std::wstring& w) {
    std::string u8 = WideToUtf8(w);
    for (char& c : u8) {
        if (c == '\\') c = '/';
    }
    return u8;
}

bool RunSftpBatch(const Session& s,
                  const std::string& commands,
                  std::string& output,
                  std::wstring* err,
                  DWORD timeoutMs) {
    output.clear();

    std::wstring sftp = FindSftpExe();
    if (sftp.empty()) {
        if (err) *err = L"没有找到 sftp.exe。请确认已安装 OpenSSH 客户端。";
        return false;
    }

    std::vector<std::wstring> args;
    args.push_back(L"-b");
    args.push_back(L"-");
    for (const auto& a : BuildSftpArgs(s)) args.push_back(a);

    std::wstring cmdline = QuoteArg(sftp);
    for (const auto& a : args) cmdline += L" " + QuoteArg(a);

    // 自动填密码的环境变量统一由 BuildAskPassEnv 生成。
    // SFTP 是另起进程、批处理模式又没有 tty，不靠 askpass 就必然
    // "Permission denied" —— 这里曾经漏了 SSH_GUI_ASKPASS 且用了反斜杠路径。
    // host key 不代答：能走到这里说明终端那边已经确认过了。
    std::vector<wchar_t> envBlock = BuildEnvironmentBlock(BuildAskPassEnv(s, false));

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inRead = nullptr, inWrite = nullptr;
    HANDLE outRead = nullptr, outWrite = nullptr;

    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
        if (err) *err = L"创建管道失败。";
        return false;
    }
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        if (err) *err = L"创建管道失败。";
        return false;
    }

    // 输入管道读端不要继承给孙进程
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;          // stderr 并到 stdout，便于统一报错
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(L'\0');

    BOOL created = CreateProcessW(
        sftp.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(), nullptr, &si, &pi);

    CloseHandle(inRead);
    CloseHandle(outWrite);

    if (!created) {
        DWORD e = GetLastError();
        CloseHandle(inWrite);
        CloseHandle(outRead);
        if (err) {
            wchar_t buf[256];
            _snwprintf_s(buf, 256, _TRUNCATE, L"无法启动 sftp.exe（错误码 %lu）。", (unsigned long)e);
            *err = buf;
        }
        return false;
    }

    // 写入命令后立刻关掉 stdin，让 sftp 知道没有更多命令
    if (!commands.empty()) {
        DWORD wrote = 0;
        WriteFile(inWrite, commands.data(), (DWORD)commands.size(), &wrote, nullptr);
    }
    CloseHandle(inWrite);

    // 读干净输出，避免子进程被管道缓冲区卡死
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(outRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
        output.append(buf, got);
        if (output.size() > 16u * 1024 * 1024) break;
    }
    CloseHandle(outRead);

    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        if (err) *err = L"操作超时（可能是网络不可达或认证卡住）。";
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code != 0 && err && err->empty()) {
        *err = L"sftp 退出码 " + std::to_wstring(code) + L"。";
    }
    return code == 0;
}

// ---------------------------------------------------------------------------
//  ls -l 输出解析
// ---------------------------------------------------------------------------
namespace {

bool NextField(const std::string& s, size_t& i, std::string& out) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size()) return false;
    size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    out = s.substr(start, i - start);
    return true;
}

// 三个字母的月份缩写（GNU ls 的日期写法）
bool IsMonthAbbrev(const std::string& s) {
    static const char* m[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (s.size() != 3) return false;
    for (const char* x : m) {
        if (_stricmp(s.c_str(), x) == 0) return true;
    }
    return false;
}

bool ParseLsLine(const std::string& raw, SftpEntry& e) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.size() < 12) return false;

    char t = line[0];
    if (t != '-' && t != 'd' && t != 'l' && t != 'c' && t != 'b' && t != 'p' && t != 's') return false;
    // 权限串至少 10 个字符
    size_t firstSp = line.find(' ');
    if (firstSp == std::string::npos || firstSp < 10) return false;
    std::string perms = line.substr(0, firstSp);

    size_t i = firstSp;
    std::string links, owner, group, sizeStr, f6, f7, f8;
    if (!NextField(line, i, links))   return false;
    if (!NextField(line, i, owner))   return false;
    if (!NextField(line, i, group))   return false;
    if (!NextField(line, i, sizeStr)) return false;
    if (!NextField(line, i, f6))      return false;

    // 日期有两种写法，字段数不同，不能写死：
    //   GNU/OpenSSH sftp ："Jan  1 12:00"   -> 月 日 时间（3 个字段）
    //   Android toybox  ："2024-01-01 12:00" -> 日期 时间（2 个字段）
    // 以前按 3 个字段硬解析，于是 toybox 那种格式会把文件名当成时间字段吃掉，
    // 判定为解析失败，整个目录就一项都列不出来。
    std::string when;
    if (IsMonthAbbrev(f6)) {
        if (!NextField(line, i, f7)) return false;   // day
        if (!NextField(line, i, f8)) return false;   // time
        when = f6 + " " + f7 + " " + f8;
    } else {
        if (!NextField(line, i, f7)) return false;   // time
        when = f6 + " " + f7;
    }

    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    std::string name = line.substr(i);
    if (name.empty()) return false;

    // 链接名形如 "name -> target"，只保留前半
    size_t arrow = name.find(" -> ");
    bool isLink = (t == 'l');
    if (arrow != std::string::npos) name = name.substr(0, arrow);
    if (name == "." || name == "..") return false;

    e.name   = Utf8ToWide(name);
    e.isDir  = (t == 'd');
    e.isLink = isLink;
    e.size   = strtoull(sizeStr.c_str(), nullptr, 10);
    e.date   = Utf8ToWide(when);
    return true;
}

// sftp 自己可能打印的提示/错误行，不要当目录项
bool LooksLikeNoise(const std::string& line) {
    static const char* noise[] = {
        "sftp>", "Connected to", "Changing to", "Remote working directory",
        "Permission denied", "No such file", "not found", "Connection closed",
        "Lost connection", "Couldn't", "Invalid", "usage:", "stat",
    };
    for (const char* n : noise) {
        if (line.find(n) != std::string::npos) return true;
    }
    return false;
}

std::wstring FirstErrorLine(const std::string& output) {
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        std::string line = output.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && LooksLikeNoise(line) && line.compare(0, 5, "sftp>") != 0) {
            return Utf8ToWide(line);
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::wstring();
}

} // namespace

bool SftpListDir(const Session& s, const std::string& path,
                 std::vector<SftpEntry>& out, std::string& absPath, std::wstring* err) {
    out.clear();
    absPath.clear();

    std::string target = path.empty() ? std::string(".") : path;

    std::string cmds;
    // 两条都加 '-' 前缀：批处理模式下命令失败会立刻中止整批，
    // 而 realpath 是 OpenSSH 6.4+ 才有的 sftp 命令，Android/dropbear 一类
    // 服务器上可能压根没有 —— 那样连后面的 ls 都执行不到，界面上就是一片空白。
    cmds += "-realpath " + EscapeSftpPath(target) + "\n";
    cmds += "-ls -l " + EscapeSftpPath(target) + "\n";

    std::string output;
    if (!RunSftpBatch(s, cmds, output, err)) {
        // 认证失败是最常见的一种。要分两种情况给话：压根没存密码，和
        // 存了密码但被服务器拒了 —— 后者再劝用户"去填密码"就是误导。
        if (output.find("Permission denied") != std::string::npos ||
            output.find("Authentication failed") != std::string::npos) {
            if (err) {
                if (s.password.empty()) {
                    *err = L"认证失败。文件传输是另起一个连接，终端里手输的密码不会共享给它 —— "
                           L"请「编辑」此会话，勾选「保存密码」并填入密码后重试。";
                } else {
                    *err = L"认证失败。已用保存的密码去连，但服务器拒绝了 —— "
                           L"请确认密码是否正确（在「编辑」里重输一次），"
                           L"以及该服务器是否允许密码登录。";
                }
            }
        } else if (err && err->empty()) {
            std::wstring e = FirstErrorLine(output);
            *err = e.empty() ? L"sftp 执行失败。" : e;
        }
        // 把 sftp 的原始输出完整留下来，否则排查时只剩一句无信息量的退出码
        LogLine(L"SftpListDir('%S') 失败；sftp 原始输出(%d 字节)：\n%S",
                target.c_str(), (int)output.size(), output.c_str());
        return false;
    }

    // 第一段有效输出是 realpath 的结果
    size_t pos = 0;
    bool gotAbs = false;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        std::string line = output.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        pos = (end == std::string::npos) ? output.size() : end + 1;

        if (line.empty()) continue;

        if (!gotAbs) {
            if (line[0] == '/') {
                absPath = line;
                gotAbs = true;
                continue;
            }
            if (line[0] == '-' || line[0] == 'd' || line[0] == 'l') {
                // realpath 没输出，直接进入列表
                gotAbs = true;
            } else {
                continue;
            }
        }

        if (LooksLikeNoise(line)) continue;

        SftpEntry e;
        if (ParseLsLine(line, e)) {
            if (line[0] == 'l') e.isLink = true;
            out.push_back(e);
        }
    }

    if (!gotAbs && absPath.empty()) absPath = target;
    if (absPath.empty()) absPath = target;

    // 目录优先，然后按名字排
    std::sort(out.begin(), out.end(), [](const SftpEntry& a, const SftpEntry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.name < b.name;
    });

    for (auto& e : out) {
        std::string base = absPath;
        if (!base.empty() && base.back() != '/') base += '/';
        e.path = base + WideToUtf8(e.name);
    }

    // 列不出东西时把原始输出记下来：这种情况多半是服务器端没有 sftp 子系统、
    // 或者 ls 输出格式不是我们认得的（Android 上的 toybox ls 就是另一套）
    if (out.empty()) {
        LogLine(L"SftpListDir('%S') 成功但 0 项，abs='%S'；sftp 原始输出(%d 字节)：\n%S",
                target.c_str(), absPath.c_str(), (int)output.size(), output.c_str());
    }

    return true;
}

// ===========================================================================
//  SFTP 窗口
// ===========================================================================
namespace {

struct LocalEntry {
    std::wstring name;
    bool         isDir = false;
    uint64_t     size  = 0;
    FILETIME     mtime = {};
};

struct SftpTask {
    enum Kind { ListRemote, ListLocal, Transfer, Simple } kind = ListRemote;
    bool ok = true;
    std::wstring message;
    std::vector<SftpEntry> remote;
    std::vector<LocalEntry> local;
    std::string remotePath;
    std::wstring localPath;
    bool refreshBoth = false;
};

std::vector<class SftpWindow*> g_windows;

bool ListLocalDir(const std::wstring& dir, std::vector<LocalEntry>& out) {
    out.clear();
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*";

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    do {
        if (wcscmp(fd.cFileName, L".") == 0) continue;
        LocalEntry e;
        e.name = fd.cFileName;
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime;
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const LocalEntry& a, const LocalEntry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

std::wstring FormatSize(uint64_t n) {
    if (n < 1024) return std::to_wstring(n) + L" B";
    if (n < 1024ull * 1024) {
        wchar_t b[32];
        _snwprintf_s(b, 32, _TRUNCATE, L"%.1f KB", n / 1024.0);
        return b;
    }
    if (n < 1024ull * 1024 * 1024) {
        wchar_t b[32];
        _snwprintf_s(b, 32, _TRUNCATE, L"%.1f MB", n / (1024.0 * 1024.0));
        return b;
    }
    wchar_t b[32];
    _snwprintf_s(b, 32, _TRUNCATE, L"%.2f GB", n / (1024.0 * 1024.0 * 1024.0));
    return b;
}

constexpr const wchar_t* kSftpClass = L"SshGuiSftpWindow";

class SftpWindow {
public:
    bool Create(HWND parent, const Session& s);
    HWND Hwnd() const { return m_hwnd; }
    const Session& GetSession() const { return m_session; }

private:
    static LRESULT CALLBACK StaticProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);

    void Layout();
    void Paint(HDC hdcTarget);
    void OnDrawItem(const DRAWITEMSTRUCT* dis);

    void SetStatus(const std::wstring& s, bool isError = false);
    void UpdateButtons();

    void RefreshLocal();
    void RefreshRemote();
    void HandleTask(SftpTask* t);

    void EnterLocal(int index);
    void EnterRemote(int index);
    void DoUpload();
    void DoDownload();
    void DoMkdir();
    void DoDeleteRemote();
    void ChooseLocalDir();

    int  SelLocal() const;
    int  SelRemote() const;

    HWND     m_hwnd = nullptr;
    HWND     m_parent = nullptr;
    Session  m_session;
    bool     m_exited = false;

    HWND m_lbLocal = nullptr;
    HWND m_lbRemote = nullptr;
    HBRUSH m_brEdit = nullptr;
    HWND m_btnUp = nullptr, m_btnDown = nullptr, m_btnMkdir = nullptr;
    HWND m_btnDelete = nullptr, m_btnRefresh = nullptr, m_btnClose = nullptr;
    HWND m_btnOpenLocal = nullptr;

    std::wstring m_localPath;
    std::string  m_remotePath = ".";

    std::vector<LocalEntry>  m_localItems;
    std::vector<SftpEntry>   m_remoteItems;

    std::wstring m_status;
    bool         m_statusErr = false;
    std::wstring m_remoteError;   // 远程列目录失败的原因；非空时直接顶掉路径那一行显示

    std::shared_ptr<std::atomic<bool>> m_alive;
    std::atomic<bool> m_busy{false};

    RECT m_topRect = {}, m_bottomRect = {};
};

void SftpWindow::SetStatus(const std::wstring& s, bool isError) {
    m_status = s;
    m_statusErr = isError;
    if (m_hwnd) InvalidateRect(m_hwnd, &m_bottomRect, FALSE);
}

void SftpWindow::UpdateButtons() {
    bool busy = m_busy.load();
    EnableWindow(m_btnUp, !busy);
    EnableWindow(m_btnDown, !busy);
    EnableWindow(m_btnMkdir, !busy);
    EnableWindow(m_btnDelete, !busy);
    EnableWindow(m_btnRefresh, !busy);

    InvalidateRect(m_btnUp, nullptr, TRUE);
    InvalidateRect(m_btnDown, nullptr, TRUE);
    InvalidateRect(m_btnMkdir, nullptr, TRUE);
    InvalidateRect(m_btnDelete, nullptr, TRUE);
    InvalidateRect(m_btnRefresh, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
bool SftpWindow::Create(HWND parent, const Session& s) {
    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = SftpWindow::StaticProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kSftpClass;
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    m_parent = parent;
    m_session = s;
    m_alive = std::make_shared<std::atomic<bool>>(true);

    wchar_t user[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", user, MAX_PATH);
    m_localPath = (n > 0 && n < MAX_PATH) ? std::wstring(user) : L"C:\\";

    std::wstring title = L"文件传输 - " + s.DisplayName() + L"  (" + s.Target() + L")";

    int W = S(920), H = S(580);
    RECT rc = { 0, 0, W, H };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    RECT pr = {};
    GetWindowRect(parent, &pr);
    int px = pr.left + 40;
    int py = pr.top + 40;
    if (px + (rc.right - rc.left) > GetSystemMetrics(SM_CXSCREEN)) px = 0;
    if (py + (rc.bottom - rc.top) > GetSystemMetrics(SM_CYSCREEN)) py = 0;

    m_hwnd = CreateWindowExW(
        0, kSftpClass, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        px, py, rc.right - rc.left, rc.bottom - rc.top,
        parent, nullptr, hInst, this);

    return m_hwnd != nullptr;
}

LRESULT CALLBACK SftpWindow::StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SftpWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (SftpWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->m_hwnd = hwnd;
    } else {
        self = (SftpWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }

    // WM_NCDESTROY 是窗口的最后一条消息，在这里销毁 C++ 对象最安全
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        delete self;
        return r;
    }

    if (self) return self->Proc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SftpWindow::Layout() {
    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    int topH = S(58);
    int botH = S(46);
    m_topRect = { 0, 0, W, topH };
    m_bottomRect = { 0, H - botH, W, H };

    int pad = S(12);
    int bottomBarY = H - botH;
    int listTop = topH + S(6);
    int listH = bottomBarY - listTop - S(52);

    int midW = S(120);
    int colW = (W - pad * 3 - midW) / 2;
    if (colW < S(120)) colW = S(120);

    int lx = pad;
    int rx = pad * 2 + colW + midW;

    MoveWindow(m_lbLocal, lx, listTop, colW, listH, TRUE);
    MoveWindow(m_lbRemote, rx, listTop, colW, listH, TRUE);

    // 中间按钮列
    int mx = pad * 2 + colW + (midW - S(96)) / 2;
    int my = listTop + listH / 2 - S(56);
    MoveWindow(m_btnUp, mx, my, S(96), S(30), TRUE);
    MoveWindow(m_btnDown, mx, my + S(38), S(96), S(30), TRUE);

    // 下方工具行
    int by = bottomBarY - S(40);
    int bw = S(96);
    int bx = pad;
    MoveWindow(m_btnOpenLocal, bx, by, S(110), S(28), TRUE);
    bx += S(118);
    MoveWindow(m_btnMkdir, bx, by, bw, S(28), TRUE);
    bx += bw + S(8);
    MoveWindow(m_btnDelete, bx, by, bw, S(28), TRUE);

    int rw = S(88);
    MoveWindow(m_btnClose, W - pad - rw, by, rw, S(28), TRUE);
    MoveWindow(m_btnRefresh, W - pad - rw * 2 - S(8), by, rw, S(28), TRUE);

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SftpWindow::Paint(HDC hdcTarget) {
    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP bmp = CreateCompatibleBitmap(hdcTarget, W, H);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    {
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        Gfx::FillRectC(g, RectF(0, 0, (REAL)W, (REAL)H), Theme::Bg);

        // 顶栏
        RectF tr((REAL)m_topRect.left, (REAL)m_topRect.top,
                 (REAL)(m_topRect.right - m_topRect.left), (REAL)(m_topRect.bottom - m_topRect.top));
        Gfx::FillRectC(g, tr, Theme::PanelBg);
        Gfx::Line(g, tr.X, tr.GetBottom() - 0.5f, tr.GetRight(), tr.GetBottom() - 0.5f, Theme::Border);

        Font* f = Gfx::UiFont(S(12), false);
        Font* fb = Gfx::UiFont(S(12), true);

        Gfx::Text(g, L"本地", fb, Theme::Text,
                  RectF((REAL)S(12), (REAL)S(8), (REAL)S(60), (REAL)S(18)), 0, 1);
        Gfx::Text(g, m_localPath, f, Theme::TextDim,
                  RectF((REAL)S(12), (REAL)S(28), (REAL)(W / 2 - S(24)), (REAL)S(20)), 0, 1);

        Gfx::Text(g, L"远程  " + m_session.Target(), fb, Theme::Text,
                  RectF((REAL)(W / 2 + S(12)), (REAL)S(8), (REAL)(W / 2 - S(24)), (REAL)S(18)), 0, 1);
        // 出错时用错误信息顶掉路径那一行，用户一眼就能看到原因
        if (!m_remoteError.empty()) {
            Gfx::Text(g, L"✗ " + m_remoteError, f, Theme::Err,
                      RectF((REAL)(W / 2 + S(12)), (REAL)S(28), (REAL)(W / 2 - S(24)), (REAL)S(20)), 0, 1);
        } else {
            Gfx::Text(g, Utf8ToWide(m_remotePath), f, Theme::TextDim,
                      RectF((REAL)(W / 2 + S(12)), (REAL)S(28), (REAL)(W / 2 - S(24)), (REAL)S(20)), 0, 1);
        }

        // 底栏
        RectF br((REAL)m_bottomRect.left, (REAL)m_bottomRect.top,
                 (REAL)(m_bottomRect.right - m_bottomRect.left),
                 (REAL)(m_bottomRect.bottom - m_bottomRect.top));
        Gfx::FillRectC(g, br, Theme::PanelBg);
        Gfx::Line(g, br.X, br.Y + 0.5f, br.GetRight(), br.Y + 0.5f, Theme::Border);

        Gfx::Text(g, m_status, f, m_statusErr ? Theme::Err : Theme::TextDim,
                  RectF((REAL)S(12), (REAL)(br.Y + S(10)), (REAL)(W - S(24)), (REAL)S(20)), 0, 1);

        // 忙碌提示
        if (m_busy.load()) {
            Gfx::Text(g, L"处理中...", f, Theme::Warn,
                      RectF((REAL)(W / 2 - S(60)), (REAL)S(34), (REAL)S(120), (REAL)S(18)), 1, 1);
        }

        g.Flush(FlushIntentionSync);
    }

    BitBlt(hdcTarget, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

void SftpWindow::OnDrawItem(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == (UINT)-1) return;

    bool isLocal = (dis->CtlID == IDC_SFTP_LOCAL_LIST);
    RECT rc = dis->rcItem;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;

    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // 注意：ListBox 项的 rcItem 带客户区偏移（第 N 项是 top=N*itemH），
    // 不能当作 (0,0,w,h) 来画，否则所有项会叠在第一行上。
    const REAL rx = (REAL)rc.left;
    const REAL ry = (REAL)rc.top;
    const REAL w  = (REAL)(rc.right - rc.left);
    const REAL h  = (REAL)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return;

    Gfx::FillRectC(g, RectF(rx, ry, w, h), selected ? Theme::SelectionBg : Theme::EditBg);

    std::wstring name;
    std::wstring meta;
    bool isDir = false;

    if (isLocal) {
        if (dis->itemID >= m_localItems.size()) return;
        const LocalEntry& e = m_localItems[dis->itemID];
        name = e.name;
        isDir = e.isDir;
        meta = e.isDir ? L"<DIR>" : FormatSize(e.size);
    } else {
        if (dis->itemID >= m_remoteItems.size()) return;
        const SftpEntry& e = m_remoteItems[dis->itemID];
        name = e.name;
        isDir = e.isDir;
        if (e.isLink)      meta = L"<链接>";
        else if (e.isDir)  meta = L"<DIR>";
        else               meta = FormatSize(e.size);
    }

    // 图标
    REAL ix = rx + (REAL)S(6);
    REAL iy = ry + h / 2.0f - (REAL)S(6);
    SolidBrush ib(GC(isDir ? Theme::Accent : Theme::TextFaint));
    if (isDir) {
        g.FillRectangle(&ib, ix, iy + (REAL)S(2), (REAL)S(13), (REAL)S(9));
        g.FillRectangle(&ib, ix, iy, (REAL)S(6), (REAL)S(4));
    } else {
        g.FillRectangle(&ib, ix + (REAL)S(1), iy, (REAL)S(10), (REAL)S(12));
    }

    Font* f = Gfx::UiFont(S(12), false);
    Gfx::Text(g, name, f, selected ? 0xFFFFFF : Theme::Text,
              RectF(ix + (REAL)S(20), ry, w - (REAL)S(120), h), 0, 1);
    Gfx::Text(g, meta, f, Theme::TextDim,
              RectF(rx + w - (REAL)S(96), ry, (REAL)S(90), h), 2, 1);

    g.Flush(FlushIntentionSync);
}

// ---------------------------------------------------------------------------
//  后台任务
// ---------------------------------------------------------------------------
void SftpWindow::RefreshRemote() {
    if (m_busy.exchange(true)) return;
    UpdateButtons();
    SetStatus(L"正在读取远程目录 " + Utf8ToWide(m_remotePath) + L" ...");

    Session sess = m_session;
    std::string path = m_remotePath;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;

    std::thread([hwnd, alive, sess, path]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::ListRemote;
        std::wstring err;
        std::string abs;
        t->ok = SftpListDir(sess, path, t->remote, abs, &err);
        t->remotePath = abs.empty() ? path : abs;
        t->message = err;
        if (alive->load()) {
            if (!PostMessageW(hwnd, WM_APP_SFTP_RESULT, 0, (LPARAM)t)) delete t;
        } else {
            delete t;
        }
    }).detach();
}

void SftpWindow::RefreshLocal() {
    std::vector<LocalEntry> items;
    if (!ListLocalDir(m_localPath, items)) {
        SetStatus(L"无法读取本地目录：" + m_localPath, true);
        return;
    }
    m_localItems = std::move(items);
    SendMessageW(m_lbLocal, LB_RESETCONTENT, 0, 0);
    for (const auto& e : m_localItems) {
        SendMessageW(m_lbLocal, LB_ADDSTRING, 0, (LPARAM)e.name.c_str());
    }
    InvalidateRect(m_lbLocal, nullptr, TRUE);
}

void SftpWindow::HandleTask(SftpTask* t) {
    std::unique_ptr<SftpTask> holder(t);
    m_busy = false;

    if (!t->ok) {
        std::wstring msg = t->message.empty() ? L"操作失败。" : t->message;
        SetStatus(msg, true);
        if (t->kind == SftpTask::ListRemote) {
            // 列目录失败时清空列表并把原因留在界面上，
            // 不然就是"一片空白"，用户根本不知道发生了什么
            m_remoteError = msg;
            m_remoteItems.clear();
            SendMessageW(m_lbRemote, LB_RESETCONTENT, 0, 0);
        }
        UpdateButtons();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    switch (t->kind) {
    case SftpTask::ListRemote: {
        m_remoteError.clear();
        m_remotePath = t->remotePath;
        m_remoteItems = t->remote;
        SendMessageW(m_lbRemote, LB_RESETCONTENT, 0, 0);
        for (const auto& e : m_remoteItems) {
            SendMessageW(m_lbRemote, LB_ADDSTRING, 0, (LPARAM)e.name.c_str());
        }
        InvalidateRect(m_lbRemote, nullptr, TRUE);
        if (m_remoteItems.empty()) {
            SetStatus(L"远程目录读到了，但一项都没有（服务器返回的 ls 输出无法识别？）", true);
        } else {
            SetStatus(L"远程目录已更新（" + std::to_wstring(m_remoteItems.size()) + L" 项）");
        }
        break;
    }

    case SftpTask::Transfer:
        SetStatus(t->message.empty() ? L"传输完成。" : t->message);
        RefreshRemote();
        break;

    case SftpTask::Simple:
        SetStatus(t->message.empty() ? L"完成。" : t->message);
        RefreshRemote();
        break;

    default:
        break;
    }

    UpdateButtons();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  操作
// ---------------------------------------------------------------------------
int SftpWindow::SelLocal() const {
    return (int)SendMessageW(m_lbLocal, LB_GETCURSEL, 0, 0);
}

int SftpWindow::SelRemote() const {
    return (int)SendMessageW(m_lbRemote, LB_GETCURSEL, 0, 0);
}

void SftpWindow::EnterLocal(int index) {
    if (index < 0 || index >= (int)m_localItems.size()) return;
    const LocalEntry& e = m_localItems[(size_t)index];
    if (!e.isDir) return;

    // ".." 单独处理：GetFullPathNameW 不会折叠它
    if (e.name == L"..") {
        if (m_localPath.size() <= 3) return;              // 已在盘符根
        size_t pos = m_localPath.find_last_of(L'\\');
        if (pos == std::wstring::npos) return;
        if (pos <= 2) m_localPath = m_localPath.substr(0, 3);
        else          m_localPath = m_localPath.substr(0, pos);
        RefreshLocal();
        return;
    }

    std::wstring np = m_localPath;
    if (!np.empty() && np.back() != L'\\') np += L'\\';
    np += e.name;

    wchar_t full[MAX_PATH * 2] = {};
    if (GetFullPathNameW(np.c_str(), MAX_PATH * 2, full, nullptr)) {
        m_localPath = full;
    } else {
        m_localPath = np;
    }
    RefreshLocal();
}

void SftpWindow::EnterRemote(int index) {
    if (index < 0 || index >= (int)m_remoteItems.size()) return;
    const SftpEntry& e = m_remoteItems[(size_t)index];
    if (!e.isDir) return;
    m_remotePath = e.path;
    RefreshRemote();
}

void SftpWindow::ChooseLocalDir() {
    BROWSEINFOW bi = {};
    bi.hwndOwner = m_hwnd;
    bi.lpszTitle = L"选择本地目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t path[MAX_PATH * 2] = {};
    if (SHGetPathFromIDListW(pidl, path)) {
        m_localPath = path;
        RefreshLocal();
    }
    CoTaskMemFree(pidl);
}

void SftpWindow::DoUpload() {
    int sel = SelLocal();
    if (sel < 0 || sel >= (int)m_localItems.size()) {
        SetStatus(L"请先在左侧选中要上传的文件。", true);
        return;
    }
    const LocalEntry& e = m_localItems[(size_t)sel];
    if (e.isDir) {
        SetStatus(L"暂不支持上传整个目录，请进入目录后逐个上传文件。", true);
        return;
    }
    if (m_busy.exchange(true)) return;
    UpdateButtons();

    std::wstring localFile = m_localPath;
    if (!localFile.empty() && localFile.back() != L'\\') localFile += L'\\';
    localFile += e.name;

    std::string remoteDir = m_remotePath;
    std::string remoteFile = remoteDir;
    if (!remoteFile.empty() && remoteFile.back() != '/') remoteFile += '/';
    remoteFile += WideToUtf8(e.name);

    SetStatus(L"正在上传 " + e.name + L" ...");

    Session sess = m_session;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;

    std::thread([hwnd, alive, sess, localFile, remoteFile, name = e.name]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::Transfer;

        std::string cmds = "put " + EscapeSftpPath(ToRemotePath(localFile)) + " " +
                           EscapeSftpPath(remoteFile) + "\n";
        std::string output;
        std::wstring err;
        t->ok = RunSftpBatch(sess, cmds, output, &err, 10 * 60 * 1000);
        if (t->ok) {
            t->message = L"上传完成：" + name;
        } else {
            std::wstring e2 = FirstErrorLine(output);
            t->message = e2.empty() ? (err.empty() ? L"上传失败。" : err) : e2;
        }

        if (alive->load()) {
            if (!PostMessageW(hwnd, WM_APP_SFTP_RESULT, 0, (LPARAM)t)) delete t;
        } else {
            delete t;
        }
    }).detach();
}

void SftpWindow::DoDownload() {
    int sel = SelRemote();
    if (sel < 0 || sel >= (int)m_remoteItems.size()) {
        SetStatus(L"请先在右侧选中要下载的文件。", true);
        return;
    }
    const SftpEntry& e = m_remoteItems[(size_t)sel];
    if (e.isDir) {
        SetStatus(L"暂不支持下载整个目录，请进入目录后逐个下载文件。", true);
        return;
    }
    if (m_busy.exchange(true)) return;
    UpdateButtons();

    std::wstring localDir = m_localPath;
    std::wstring localFile = localDir;
    if (!localFile.empty() && localFile.back() != L'\\') localFile += L'\\';
    localFile += e.name;

    SetStatus(L"正在下载 " + e.name + L" ...");

    Session sess = m_session;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;
    std::string remoteFile = e.path;

    std::thread([hwnd, alive, sess, localFile, remoteFile, name = e.name]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::Transfer;

        std::string cmds = "get " + EscapeSftpPath(remoteFile) + " " +
                           EscapeSftpPath(ToRemotePath(localFile)) + "\n";
        std::string output;
        std::wstring err;
        t->ok = RunSftpBatch(sess, cmds, output, &err, 10 * 60 * 1000);
        if (t->ok) {
            t->message = L"下载完成：" + name;
        } else {
            std::wstring e2 = FirstErrorLine(output);
            t->message = e2.empty() ? (err.empty() ? L"下载失败。" : err) : e2;
        }

        if (alive->load()) {
            if (!PostMessageW(hwnd, WM_APP_SFTP_RESULT, 0, (LPARAM)t)) delete t;
        } else {
            delete t;
        }
    }).detach();
}

void SftpWindow::DoMkdir() {
    wchar_t buf[256] = {};
    // 用简单的输入框：借道 InputBox 不可用，这里用一个小对话框
    struct Ctx { wchar_t* out; size_t cch; } ctx{ buf, 256 };

    // 简化：用 Windows 的 IFileDialog 太啰嗦，直接弹一个自绘小窗
    // 这里退而求其次，用 CreateWindow 的 EDIT 组合过于冗长，改用命令行式的输入
    // —— 最终选择：用 MessageBox 提示用户改用命令行，或直接用默认名。
    // 为保持功能完整，这里用 GetSaveFileName 的输入能力不合适，故使用简易 InputBox：
    extern bool SimpleInputBox(HWND parent, const wchar_t* title, const wchar_t* prompt,
                               wchar_t* buf, size_t cch);
    if (!SimpleInputBox(m_hwnd, L"新建远程目录", L"目录名：", ctx.out, ctx.cch)) return;
    if (!buf[0]) return;

    if (m_busy.exchange(true)) return;
    UpdateButtons();
    SetStatus(L"正在创建远程目录...");

    std::string dirName = WideToUtf8(buf);
    std::string target = m_remotePath;
    if (!target.empty() && target.back() != '/') target += '/';
    target += dirName;

    Session sess = m_session;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;

    std::thread([hwnd, alive, sess, target, dirName]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::Simple;

        std::string cmds = "mkdir " + EscapeSftpPath(target) + "\n";
        std::string output;
        std::wstring err;
        t->ok = RunSftpBatch(sess, cmds, output, &err);
        if (t->ok) {
            t->message = L"已创建目录：" + Utf8ToWide(dirName);
        } else {
            std::wstring e2 = FirstErrorLine(output);
            t->message = e2.empty() ? (err.empty() ? L"创建目录失败。" : err) : e2;
        }

        if (alive->load()) {
            if (!PostMessageW(hwnd, WM_APP_SFTP_RESULT, 0, (LPARAM)t)) delete t;
        } else {
            delete t;
        }
    }).detach();
}

void SftpWindow::DoDeleteRemote() {
    int sel = SelRemote();
    if (sel < 0 || sel >= (int)m_remoteItems.size()) {
        SetStatus(L"请先在右侧选中要删除的项目。", true);
        return;
    }
    const SftpEntry& e = m_remoteItems[(size_t)sel];

    std::wstring msg = L"确定要删除远程" + std::wstring(e.isDir ? L"目录" : L"文件") +
                       L"「" + e.name + L"」吗？\n\n" + Utf8ToWide(e.path) + L"\n\n此操作不可撤销。";
    if (MessageBoxW(m_hwnd, msg.c_str(), L"删除远程项目",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (m_busy.exchange(true)) return;
    UpdateButtons();
    SetStatus(L"正在删除...");

    std::string target = e.path;
    bool isDir = e.isDir;
    Session sess = m_session;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;

    std::thread([hwnd, alive, sess, target, isDir]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::Simple;

        // 目录用 rmdir，文件用 rm
        std::string cmds = std::string(isDir ? "rmdir " : "rm ") + EscapeSftpPath(target) + "\n";
        std::string output;
        std::wstring err;
        t->ok = RunSftpBatch(sess, cmds, output, &err);
        if (t->ok) {
            t->message = L"已删除。";
        } else {
            std::wstring e2 = FirstErrorLine(output);
            t->message = e2.empty() ? (err.empty() ? L"删除失败。" : err) : e2;
        }

        if (alive->load()) {
            if (!PostMessageW(hwnd, WM_APP_SFTP_RESULT, 0, (LPARAM)t)) delete t;
        } else {
            delete t;
        }
    }).detach();
}

// ---------------------------------------------------------------------------
LRESULT SftpWindow::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        HFONT font = Gfx::MakeUiFont(S(13), false);
        m_brEdit = CreateSolidBrush(Rgb(Theme::EditBg));

        auto mkList = [&](int id) {
            HWND h = CreateWindowExW(
                0, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                    LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                0, 0, 10, 10, m_hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
            if (h) {
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                // 显式定高，别只指望 WM_MEASUREITEM（它是在 CreateWindowEx 期间
                // 发的，稍有闪失项高就会退化成整块列表那么高）
                SendMessageW(h, LB_SETITEMHEIGHT, 0, (LPARAM)S(24));
            }
            return h;
        };

        m_lbLocal  = mkList(IDC_SFTP_LOCAL_LIST);
        m_lbRemote = mkList(IDC_SFTP_REMOTE_LIST);

        auto mkBtn = [&](int id, const wchar_t* text) {
            HWND h = CreateWindowExW(
                0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 10, 10, m_hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
            if (h) {
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                TrackButtonHover(h);
            }
            return h;
        };

        m_btnUp        = mkBtn(IDC_SFTP_UP, L"上传  >>>");
        m_btnDown      = mkBtn(IDC_SFTP_DOWN, L"<<<  下载");
        m_btnMkdir     = mkBtn(IDC_SFTP_MKDIR, L"新建目录");
        m_btnDelete    = mkBtn(IDC_SFTP_DELETE, L"删除远程");
        m_btnRefresh   = mkBtn(IDC_SFTP_REFRESH, L"刷新");
        m_btnClose     = mkBtn(IDC_SFTP_CLOSE, L"关闭");
        m_btnOpenLocal = mkBtn(IDC_SFTP_OPEN_LOCAL, L"选择本地目录...");

        Layout();
        RefreshLocal();
        RefreshRemote();
        UpdateButtons();
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

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
        if (mis && (mis->CtlID == IDC_SFTP_LOCAL_LIST || mis->CtlID == IDC_SFTP_REMOTE_LIST)) {
            mis->itemHeight = (UINT)S(24);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (!dis) return FALSE;

        if (dis->CtlID == IDC_SFTP_LOCAL_LIST || dis->CtlID == IDC_SFTP_REMOTE_LIST) {
            OnDrawItem(dis);
            return TRUE;
        }

        BtnStyle style = BtnStyle::Normal;
        if (dis->CtlID == IDC_SFTP_UP || dis->CtlID == IDC_SFTP_DOWN) style = BtnStyle::Primary;
        else if (dis->CtlID == IDC_SFTP_DELETE) style = BtnStyle::Danger;
        else if (dis->CtlID == IDC_SFTP_CLOSE || dis->CtlID == IDC_SFTP_OPEN_LOCAL)
            style = BtnStyle::Ghost;

        DrawButton(dis, ButtonHovered(dis->hwndItem), style);
        return TRUE;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, Rgb(Theme::Text));
        SetBkColor(hdc, Rgb(Theme::EditBg));
        // 必须是真画刷：返回 NULL_BRUSH 等于不擦背景，列表会露出系统白底
        return (LRESULT)(m_brEdit ? m_brEdit : GetStockObject(BLACK_BRUSH));
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        switch (id) {
        case IDC_SFTP_LOCAL_LIST:
            if (code == LBN_DBLCLK) EnterLocal(SelLocal());
            return 0;
        case IDC_SFTP_REMOTE_LIST:
            if (code == LBN_DBLCLK) EnterRemote(SelRemote());
            return 0;
        case IDC_SFTP_UP:        DoUpload();       return 0;
        case IDC_SFTP_DOWN:      DoDownload();     return 0;
        case IDC_SFTP_MKDIR:     DoMkdir();        return 0;
        case IDC_SFTP_DELETE:    DoDeleteRemote(); return 0;
        case IDC_SFTP_REFRESH:   RefreshLocal(); RefreshRemote(); return 0;
        case IDC_SFTP_OPEN_LOCAL: ChooseLocalDir(); return 0;
        case IDC_SFTP_CLOSE:     DestroyWindow(m_hwnd); return 0;
        default: break;
        }
        break;
    }

    case WM_APP_SFTP_RESULT:
        HandleTask((SftpTask*)lp);
        return 0;

    case WM_CLOSE:
        DestroyWindow(m_hwnd);
        return 0;

    case WM_DESTROY: {
        if (m_alive) m_alive->store(false);
        if (m_brEdit) { DeleteObject(m_brEdit); m_brEdit = nullptr; }
        for (size_t i = 0; i < g_windows.size(); ++i) {
            if (g_windows[i] == this) {
                g_windows.erase(g_windows.begin() + i);
                break;
            }
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
static SftpWindow* FindWindowFor(const Session& s) {
    for (auto* w : g_windows) {
        if (w->Hwnd() && w->GetSession().host == s.host &&
            w->GetSession().port == s.port && w->GetSession().user == s.user) {
            return w;
        }
    }
    return nullptr;
}

void OpenSftpWindow(HWND parent, const Session& s) {
    if (FindSftpExe().empty()) {
        MessageBoxW(parent,
                    L"没有找到 sftp.exe。\n\n"
                    L"请安装 Windows 的 OpenSSH 客户端：\n"
                    L"  设置 -> 系统 -> 可选功能 -> 添加功能 -> OpenSSH 客户端",
                    L"无法进行文件传输", MB_ICONWARNING);
        return;
    }

    if (SftpWindow* existing = FindWindowFor(s)) {
        ShowWindow(existing->Hwnd(), SW_SHOW);
        SetForegroundWindow(existing->Hwnd());
        return;
    }

    auto* w = new SftpWindow();
    if (!w->Create(parent, s)) {
        delete w;
        MessageBoxW(parent, L"创建文件传输窗口失败。", L"错误", MB_ICONERROR);
        return;
    }

    g_windows.push_back(w);
    ApplyDarkTitleBar(w->Hwnd());
    ShowWindow(w->Hwnd(), SW_SHOW);
    UpdateWindow(w->Hwnd());
}

void CloseAllSftpWindows() {
    std::vector<SftpWindow*> copy = g_windows;
    for (auto* w : copy) {
        if (w->Hwnd()) DestroyWindow(w->Hwnd());
    }
    g_windows.clear();
}
