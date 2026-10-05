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
    IDC_SFTP_REFRESH_LOCAL,
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

namespace {
// 实现在下面同一个匿名命名空间里；RunSftpBatch 要用，所以先声明
std::wstring FirstErrorLine(const std::string& output);
bool LooksLikeCommandFailure(const std::string& output);
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

    // 千万不要加 "-b -"（批处理模式）。
    //
    // sftp 的 -b 会在解析完命令行之后强制 options.batch_mode = 1，位置在
    // "-o BatchMode=no" 之后，所以那个 -o 根本覆盖不了它。而 BatchMode=yes
    // 的含义是"禁止一切密码询问" —— ssh 连 password 认证都不会尝试，直接
    // 报 Permission denied (publickey,...)。这个坑很隐蔽：同样的密码和
    // askpass 环境变量，用 ssh 能登录成功，用 sftp -b 必失败。
    //
    // 改成不带 -b：直接把命令写进 stdin（交互模式）。输出的 "sftp> " 提示符
    // 和 "Connected to ..." 由 LooksLikeNoise 过滤掉。
    std::vector<std::wstring> args;
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

    // 写入命令后立刻关掉 stdin，让 sftp 知道没有更多命令。
    // 末尾补一个 quit：不带 -b 时 sftp 是交互模式，明确退出比依赖 EOF 更稳。
    if (!commands.empty()) {
        std::string all = commands;
        if (all.empty() || all.back() != '\n') all += '\n';
        all += "quit\n";
        DWORD wrote = 0;
        WriteFile(inWrite, all.data(), (DWORD)all.size(), &wrote, nullptr);
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

    bool ok = (code == 0);

    // 交互模式下单条命令失败**不会**改变退出码 —— 最后那条 quit 照样让 sftp
    // 返回 0。所以光看退出码会把"新建目录失败""删除失败"全当成成功，
    // 还得从输出里认失败迹象。
    if (ok && LooksLikeCommandFailure(output)) {
        ok = false;
        if (err && err->empty()) {
            std::wstring line = FirstErrorLine(output);
            *err = line.empty() ? L"远程命令执行失败。" : line;
        }
    }

    if (!ok && err && err->empty()) {
        *err = L"sftp 退出码 " + std::to_wstring(code) + L"。";
    }
    return ok;
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
    // 只认真正像错误的措辞。
    // 这里**不能复用 LooksLikeNoise** —— 那个名单是给"别把提示符/问候语当成目录项"
    // 用的，里面既有错误（Permission denied）也有完全正常的行
    // （Connected to / Remote working directory）。拿它当错误行用，就会把
    // "Connected to 192.168.1.225." 这种问候语报给用户当失败原因。
    static const char* marks[] = {
        "Permission denied", "No such file", "not found", "Connection closed",
        "Lost connection", "Couldn't", "Invalid command", "usage:",
        "Failure", "not a directory", "Bad message", "File not found",
        "remote readdir", "remote mkdir", "remote rmdir", "remote rename",
        "remote remove", "No space left", "Quota exceeded", "stat remote",
    };
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        std::string line = output.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line.compare(0, 5, "sftp>") != 0) {
            for (const char* m : marks) {
                if (line.find(m) != std::string::npos) return Utf8ToWide(line);
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return std::wstring();
}

// 交互模式下单条命令失败不改退出码，只能从输出里认。
// 这里只认 sftp 自己那几种固定措辞，避免把文件名的巧合当成失败。
bool LooksLikeCommandFailure(const std::string& output) {
    static const char* marks[] = {
        "\": Failure",              // remote mkdir "/x": Failure
        "remote mkdir",             // 同上（前缀）
        "remote rmdir",
        "remote rename",
        "remote remove",
        "remote readdir",           // 目录存在但无权读，ls 会报这个
        "Couldn't ",                // Couldn't stat / Couldn't read directory
        "No such file or directory",
        "Permission denied",
        "not a directory",
        "Invalid command",
        "Bad message",
        "File not found",
    };
    for (const char* m : marks) {
        if (output.find(m) != std::string::npos) return true;
    }
    return false;
}

// 取父目录。根目录的父目录仍是根 —— 免得拼出 "//" 或空串。
std::string ParentPath(const std::string& p) {
    if (p.empty() || p == "/") return "/";
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t slash = s.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return s.substr(0, slash);
}

} // namespace

bool SftpListDir(const Session& s, const std::string& path,
                 std::vector<SftpEntry>& out, std::string& absPath, std::wstring* err) {
    out.clear();
    absPath.clear();

    std::string target = path.empty() ? std::string(".") : path;

    std::string cmds;
    // 别用 realpath：它是**批处理模式**专有的命令，交互模式下会直接回
    // "Invalid command."，路径就永远停在初始的 "."。
    // 改用 cd + pwd 拿绝对路径；ls -l 不带参数即列当前目录，
    // 返回的文件名不带 "./" 前缀，拼出来的 e.path 才干净。
    cmds += "cd " + EscapeSftpPath(target) + "\n";
    cmds += "pwd\n";
    cmds += "ls -l\n";

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
        } else if (output.find("remote readdir") != std::string::npos ||
                   output.find("Couldn't read directory") != std::string::npos) {
            // 目录确实存在、cd 也成功了，只是服务器不让读它 —— 典型是 Android 的
            // /storage/emulated 这种只给 execute 不给 read 的目录。
            // 直接把 "remote readdir(...): No such file or directory" 甩给用户，
            // 既看不懂、又像是程序坏了。
            if (err) *err = L"服务器不允许列出这个目录（通常是权限限制）。";
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

        // 提示符行要先剔除。否则 "sftp> pwd" 的首字符 's' 会被下面的类型判断
        // 当成 socket 文件行，gotAbs 提前置位，真正的 pwd 输出反而被跳过。
        if (line.compare(0, 5, "sftp>") == 0) continue;

        if (!gotAbs) {
            // pwd 的输出形如 "Remote working directory: /data/data/..."
            static const std::string kPwdPrefix = "Remote working directory:";
            if (line.compare(0, kPwdPrefix.size(), kPwdPrefix) == 0) {
                std::string p = line.substr(kPwdPrefix.size());
                while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
                while (!p.empty() && (p.back() == ' ' || p.back() == '\r')) p.pop_back();
                if (!p.empty() && p[0] == '/') {
                    absPath = p;
                    gotAbs = true;
                }
                continue;
            }
            if (line[0] == '-' || line[0] == 'd' || line[0] == 'l' || line[0] == 'c' ||
                line[0] == 'b' || line[0] == 'p' || line[0] == 's') {
                // 没拿到 pwd 就先按原样进列表
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

    // sftp 的 ls 不返回 . 和 ..（列的就是目录内容），所以自己补一个 ".."，
    // 否则进了深层目录就只能靠关掉窗口重开才能回上级。
    // 不在根目录时才加；path 直接算成父目录，省得拼出 "/a/b/.." 这种。
    if (!absPath.empty() && absPath != "/") {
        SftpEntry up;
        up.name  = L"..";
        up.isDir = true;
        up.path  = ParentPath(absPath);
        out.push_back(up);
    }

    // 目录优先，然后按名字排。'..' 是目录且名字以 '.' 开头，自然落在最前。
    std::sort(out.begin(), out.end(), [](const SftpEntry& a, const SftpEntry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.name < b.name;
    });

    for (auto& e : out) {
        if (e.name == L"..") continue;   // 上面已经算好父目录路径
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
    // 传输完成后的自动刷新用：不要把状态栏改成"正在读取/已更新"，
    // 否则用户刚看到的"上传完成：N 个文件"会立刻被冲掉。
    bool quiet = false;
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
    void RefreshRemoteTo(const std::string& target, bool quiet = false);
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
    // 多选：返回所有选中项（升序）。Ctrl / Shift 点击由 ListBox 的
    // LBS_EXTENDEDSEL 样式处理，这里只负责把选中集合取出来。
    std::vector<int> SelLocalItems() const;
    std::vector<int> SelRemoteItems() const;
    // 选中项变化时在状态栏报一句"选中 N 项"，多选时用户才知道自己选了几个
    void ReportSelection();

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
    HWND m_btnRefreshLocal = nullptr;

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

    // ---- 多标签页 ----------------------------------------------------------
    // 两侧各自一组标签，每个标签记住自己的目录。m_localPath / m_remotePath
    // 始终代表**当前标签**的路径，只在切换标签的那一刻与标签数组同步 ——
    // 这样导航、上传下载那些代码完全不必知道标签的存在。
    struct PaneTab {
        std::wstring localPath;
        std::string  remotePath;
    };
    std::vector<PaneTab> m_localTabs;
    std::vector<PaneTab> m_remoteTabs;
    int  m_localActive  = 0;
    int  m_remoteActive = 0;

    RECT m_localTabsRect = {}, m_remoteTabsRect = {};
    int  m_hoverTab      = -1;    // 悬停的标签下标
    int  m_hoverSide     = 0;     // 0=无 1=本地 2=远程
    bool m_hoverClose    = false; // 悬停在关闭叉上
    bool m_hoverPlus     = false; // 悬停在 "+" 上

    void SwitchLocalTab(int idx);
    void SwitchRemoteTab(int idx);
    void AddLocalTab();
    void AddRemoteTab();
    void CloseLocalTab(int idx);
    void CloseRemoteTab(int idx);
    void DrawTabStrip(Gdiplus::Graphics& g, const RECT& rc, bool local);
    int  HitTestTabs(const RECT& rc, int count, int x, int y, bool* onClose, bool* onPlus);
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
//  多标签页
// ---------------------------------------------------------------------------
namespace {

// 标签栏的几何：固定宽度，标题长了自己省略 —— 布局与命中测试共用同一套算法，
// 免得出现"看着在这里、点下去是另一个"。
constexpr int kTabW      = 118;   // 逻辑像素
constexpr int kTabPlusW  = 26;
constexpr int kTabGap    = 2;

int TabStripStep() { return S(kTabW) + S(kTabGap); }

// 本地路径取个短标题：最后一段；盘符根就显示盘符。
std::wstring LocalTabTitle(const std::wstring& p) {
    if (p.size() <= 3) return p.empty() ? L"?" : p;
    size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos + 1 >= p.size()) return p;
    std::wstring t = p.substr(pos + 1);
    return t.empty() ? p : t;
}

// 远程路径同理。"/" 显示成 "/"。
std::wstring RemoteTabTitle(const std::string& p) {
    if (p.empty()) return L"?";
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s == "/") return L"/";
    size_t pos = s.find_last_of('/');
    std::wstring t = Utf8ToWide(pos == std::string::npos ? s : s.substr(pos + 1));
    return t.empty() ? L"/" : t;
}

} // namespace

int SftpWindow::HitTestTabs(const RECT& rc, int count, int x, int y,
                            bool* onClose, bool* onPlus) {
    if (onClose) *onClose = false;
    if (onPlus)  *onPlus  = false;
    if (y < rc.top || y >= rc.bottom) return -1;

    int step = TabStripStep();
    int first = rc.left + S(4);
    for (int i = 0; i < count; ++i) {
        int left = first + i * step;
        if (x >= left && x < left + S(kTabW)) {
            // 激活标签的右端多画了一个 ×，命中区也划在那里
            if (onClose && x >= left + S(kTabW) - S(20)) *onClose = true;
            return i;
        }
    }
    int plusLeft = first + count * step + S(4);
    if (x >= plusLeft && x < plusLeft + S(kTabPlusW)) {
        if (onPlus) *onPlus = true;
    }
    return -1;
}

void SftpWindow::SwitchLocalTab(int idx) {
    if (idx < 0 || idx >= (int)m_localTabs.size() || idx == m_localActive) return;
    m_localTabs[(size_t)m_localActive].localPath = m_localPath;   // 存回当前
    m_localActive = idx;
    m_localPath = m_localTabs[(size_t)idx].localPath;             // 载入目标
    RefreshLocal();
    InvalidateRect(m_hwnd, &m_localTabsRect, FALSE);
}

void SftpWindow::SwitchRemoteTab(int idx) {
    if (idx < 0 || idx >= (int)m_remoteTabs.size() || idx == m_remoteActive) return;
    m_remoteTabs[(size_t)m_remoteActive].remotePath = m_remotePath;
    m_remoteActive = idx;
    m_remotePath = m_remoteTabs[(size_t)idx].remotePath;
    m_remoteError.clear();
    RefreshRemote();
    InvalidateRect(m_hwnd, &m_remoteTabsRect, FALSE);
}

void SftpWindow::AddLocalTab() {
    m_localTabs[(size_t)m_localActive].localPath = m_localPath;
    PaneTab t;
    t.localPath = m_localPath;     // 新标签停在同一个目录，多数时候正是想要的
    m_localTabs.push_back(t);
    m_localActive = (int)m_localTabs.size() - 1;
    RefreshLocal();
    InvalidateRect(m_hwnd, &m_localTabsRect, FALSE);
}

void SftpWindow::AddRemoteTab() {
    m_remoteTabs[(size_t)m_remoteActive].remotePath = m_remotePath;
    PaneTab t;
    t.remotePath = m_remotePath;
    m_remoteTabs.push_back(t);
    m_remoteActive = (int)m_remoteTabs.size() - 1;
    m_remoteError.clear();
    RefreshRemote();
    InvalidateRect(m_hwnd, &m_remoteTabsRect, FALSE);
}

void SftpWindow::CloseLocalTab(int idx) {
    if (idx < 0 || idx >= (int)m_localTabs.size()) return;
    if (m_localTabs.size() <= 1) return;          // 至少留一个，不然这一侧就空了
    m_localTabs.erase(m_localTabs.begin() + idx);
    if (idx < m_localActive) {
        --m_localActive;
    } else if (idx == m_localActive) {
        if (m_localActive >= (int)m_localTabs.size()) m_localActive = (int)m_localTabs.size() - 1;
        m_localPath = m_localTabs[(size_t)m_localActive].localPath;
        RefreshLocal();
    }
    InvalidateRect(m_hwnd, &m_localTabsRect, FALSE);
}

void SftpWindow::CloseRemoteTab(int idx) {
    if (idx < 0 || idx >= (int)m_remoteTabs.size()) return;
    if (m_remoteTabs.size() <= 1) return;
    m_remoteTabs.erase(m_remoteTabs.begin() + idx);
    if (idx < m_remoteActive) {
        --m_remoteActive;
    } else if (idx == m_remoteActive) {
        if (m_remoteActive >= (int)m_remoteTabs.size()) m_remoteActive = (int)m_remoteTabs.size() - 1;
        m_remotePath = m_remoteTabs[(size_t)m_remoteActive].remotePath;
        m_remoteError.clear();
        RefreshRemote();
    }
    InvalidateRect(m_hwnd, &m_remoteTabsRect, FALSE);
}

void SftpWindow::DrawTabStrip(Gdiplus::Graphics& g, const RECT& rc, bool local) {
    const std::vector<PaneTab>& tabs = local ? m_localTabs : m_remoteTabs;
    int active = local ? m_localActive : m_remoteActive;
    int side   = local ? 1 : 2;

    Gfx::FillRectC(g, RectF((REAL)rc.left, (REAL)rc.top,
                            (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top)),
                   Theme::PanelBg);
    Gfx::Line(g, (REAL)rc.left, (REAL)rc.bottom - 0.5f, (REAL)rc.right,
              (REAL)rc.bottom - 0.5f, Theme::Border);

    Font* f  = Gfx::UiFont(S(12), false);
    Font* fb = Gfx::UiFont(S(12), true);

    int step   = TabStripStep();
    int first  = rc.left + S(4);
    int top    = rc.top + S(4);
    int height = (rc.bottom - rc.top) - S(6);

    for (int i = 0; i < (int)tabs.size(); ++i) {
        int left = first + i * step;
        RECT tr = { left, top, left + S(kTabW), top + height };
        bool isActive = (i == active);
        bool hovered  = (m_hoverSide == side && m_hoverTab == i);

        uint32_t bg = isActive ? Theme::TabActive : (hovered ? Theme::TabHover : Theme::TabIdle);
        Gfx::FillRectC(g, RectF((REAL)tr.left, (REAL)tr.top,
                                (REAL)(tr.right - tr.left), (REAL)(tr.bottom - tr.top)), bg);

        // 激活标签顶上一条高亮，和终端标签页的观感一致
        if (isActive) {
            Gfx::FillRectC(g, RectF((REAL)tr.left, (REAL)tr.top,
                                    (REAL)(tr.right - tr.left), (REAL)S(2)), Theme::Accent);
        }
        Gfx::Line(g, (REAL)tr.right - 0.5f, (REAL)tr.top + S(3),
                  (REAL)tr.right - 0.5f, (REAL)tr.bottom - S(3), Theme::Border);

        std::wstring title = local ? LocalTabTitle(tabs[(size_t)i].localPath)
                                   : RemoteTabTitle(tabs[(size_t)i].remotePath);
        // 激活标签右边要留出 × 的位置
        REAL textW = (REAL)(tr.right - tr.left) - (isActive ? S(26) : S(12));
        Gfx::Text(g, title, isActive ? fb : f,
                  isActive ? Theme::Text : Theme::TextDim,
                  RectF((REAL)tr.left + S(6), (REAL)tr.top, textW,
                        (REAL)(tr.bottom - tr.top)),
                  0, 1);

        if (isActive) {
            // 关闭叉
            bool closeHover = (m_hoverSide == side && m_hoverTab == i && m_hoverClose);
            RectF cr((REAL)(tr.right - S(19)), (REAL)(tr.top + (height - S(14)) / 2),
                     (REAL)S(14), (REAL)S(14));
            Gfx::Text(g, L"\x00D7", f, closeHover ? Theme::Err : Theme::TextFaint,
                      cr, 0, 1);
        }
    }

    // "+" 新建标签
    int plusLeft = first + (int)tabs.size() * step + S(4);
    RECT pr = { plusLeft, top, plusLeft + S(kTabPlusW), top + height };
    bool plusHover = (m_hoverSide == side && m_hoverPlus);
    Gfx::FillRectC(g, RectF((REAL)pr.left, (REAL)pr.top,
                            (REAL)(pr.right - pr.left), (REAL)(pr.bottom - pr.top)),
                   plusHover ? Theme::BtnHover : Theme::TabIdle);
    Gfx::Text(g, L"+", fb, plusHover ? Theme::Text : Theme::TextDim,
              RectF((REAL)pr.left, (REAL)pr.top, (REAL)(pr.right - pr.left),
                    (REAL)(pr.bottom - pr.top)), 0, 1);
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

    int W = S(920), H = S(608);   // 608 = 580 + 28，把标签栏那一条的高度补回来
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

    int tabsH = S(28);
    int topH = S(58) + tabsH;          // 58 = 标题 + 路径两行，下面接标签栏
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

    m_localTabsRect  = { lx, S(58), lx + colW, S(58) + tabsH };
    m_remoteTabsRect = { rx, S(58), rx + colW, S(58) + tabsH };

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
    MoveWindow(m_btnRefreshLocal, bx, by, bw, S(28), TRUE);
    bx += bw + S(8);
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

        // 标签栏（在路径行下面，两边各一条）
        DrawTabStrip(g, m_localTabsRect, true);
        DrawTabStrip(g, m_remoteTabsRect, false);

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
    RefreshRemoteTo(m_remotePath);
}

// 列指定目录。**成功之前不动 m_remotePath** —— 否则进了列不出来的目录
// （Android 的 /storage/emulated 就是：能 cd 进去但无权 readdir）之后，
// 路径已经改了、列表又是空的，用户会卡在一个空窗口里连 ".." 都没有。
void SftpWindow::RefreshRemoteTo(const std::string& target, bool quiet) {
    if (m_busy.exchange(true)) return;
    UpdateButtons();
    if (!quiet) SetStatus(L"正在读取远程目录 " + Utf8ToWide(target) + L" ...");

    Session sess = m_session;
    std::string path = target;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;

    std::thread([hwnd, alive, sess, path, quiet]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::ListRemote;
        t->quiet = quiet;
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
        // 列目录失败时补上"想进的"和"当前所在的"，否则用户只看到一句报错，
        // 既不知道是谁的问题、也不知道自己现在停在哪。
        if (t->kind == SftpTask::ListRemote && !t->remotePath.empty()) {
            msg = L"无法读取 " + Utf8ToWide(t->remotePath) + L"：" + msg;
            if (!m_remotePath.empty() && m_remotePath != t->remotePath) {
                msg += L"（仍停留在 " + Utf8ToWide(m_remotePath) + L"）";
            }
        }
        SetStatus(msg, true);
        if (t->kind == SftpTask::ListRemote) {
            // 只留错误信息，**故意不清空列表**。
            // 清空之后用户既看不到内容、又没有 ".." 可以退，会卡在一个空窗口里 ——
            // Android 的 /storage/emulated 正是这种：能 cd 进去但无权 readdir。
            // 保留上一个成功列出的目录，他还能继续操作。
            // （m_remotePath 也没被改过，见 RefreshRemoteTo 的注释。）
            m_remoteError = msg;
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
        // quiet 是"传输完成后的自动刷新"，状态栏要留给那句完成提示
        if (!t->quiet) {
            if (m_remoteItems.empty()) {
                SetStatus(L"远程目录读到了，但一项都没有（服务器返回的 ls 输出无法识别？）", true);
            } else {
                SetStatus(L"远程目录已更新（" + std::to_wstring(m_remoteItems.size()) + L" 项）");
            }
        }
        break;
    }

    case SftpTask::Transfer:
        SetStatus(t->message.empty() ? L"传输完成。" : t->message);
        RefreshRemoteTo(m_remotePath, true);   // 静默刷新，别盖掉上面那句
        break;

    case SftpTask::Simple:
        SetStatus(t->message.empty() ? L"完成。" : t->message);
        RefreshRemoteTo(m_remotePath, true);
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
    // 多选模式下 LB_GETCURSEL 给的是"焦点项"（也就是锚点），
    // 双击进目录、上下键移动都靠它，正合适。
    return (int)SendMessageW(m_lbLocal, LB_GETCURSEL, 0, 0);
}

int SftpWindow::SelRemote() const {
    return (int)SendMessageW(m_lbRemote, LB_GETCURSEL, 0, 0);
}

std::vector<int> SftpWindow::SelLocalItems() const {
    std::vector<int> v;
    int n = (int)SendMessageW(m_lbLocal, LB_GETSELCOUNT, 0, 0);
    if (n <= 0) return v;
    v.resize((size_t)n);
    int got = (int)SendMessageW(m_lbLocal, LB_GETSELITEMS, (WPARAM)n, (LPARAM)v.data());
    if (got < n) v.resize((size_t)(got < 0 ? 0 : got));
    return v;
}

std::vector<int> SftpWindow::SelRemoteItems() const {
    std::vector<int> v;
    int n = (int)SendMessageW(m_lbRemote, LB_GETSELCOUNT, 0, 0);
    if (n <= 0) return v;
    v.resize((size_t)n);
    int got = (int)SendMessageW(m_lbRemote, LB_GETSELITEMS, (WPARAM)n, (LPARAM)v.data());
    if (got < n) v.resize((size_t)(got < 0 ? 0 : got));
    return v;
}

void SftpWindow::ReportSelection() {
    // 两边都报一下，用户一眼能看出各选了几个 —— 多选最容易犯的错
    // 就是以为选上了其实没选上。
    size_t nl = SelLocalItems().size();
    size_t nr = SelRemoteItems().size();
    if (nl <= 1 && nr <= 1) return;   // 单选/没选不用打扰

    std::wstring s;
    if (nl > 1) s += L"本地选中 " + std::to_wstring(nl) + L" 项";
    if (nr > 1) {
        if (!s.empty()) s += L"；";
        s += L"远程选中 " + std::to_wstring(nr) + L" 项";
    }
    SetStatus(s);
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
    // 符号链接也要能进。
    // ls -l 只能告诉我们"这是个链接"，看不出目标是文件还是目录，所以不在这里
    // 拦，直接交给 SftpListDir 去 cd：是目录就列出来，是文件会失败并报错。
    // Android 的 Termux 里 storage/ 下面（以及很多别处）全是这种链接，
    // 一律拦掉的话能操作的目录范围会小得可怜。
    if (!e.isDir && !e.isLink) return;
    // 交给 RefreshRemoteTo 去试，成功才切换路径（见它的注释）
    RefreshRemoteTo(e.path);
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
    std::vector<int> sel = SelLocalItems();
    if (sel.empty()) {
        SetStatus(L"请先在左侧选中要上传的文件（按住 Ctrl / Shift 可以多选）。", true);
        return;
    }

    // 收集待上传的文件；目录（含 ".."）跳过，最后统一报一句跳过了几个
    std::vector<std::wstring> names;
    int skippedDirs = 0;
    for (int idx : sel) {
        if (idx < 0 || idx >= (int)m_localItems.size()) continue;
        const LocalEntry& e = m_localItems[(size_t)idx];
        if (e.isDir) { ++skippedDirs; continue; }
        names.push_back(e.name);
    }
    if (names.empty()) {
        SetStatus(L"选中的都是目录。暂不支持上传整个目录，请进入目录后选文件。", true);
        return;
    }
    if (m_busy.exchange(true)) return;
    UpdateButtons();

    std::wstring localDir = m_localPath;
    if (!localDir.empty() && localDir.back() != L'\\') localDir += L'\\';
    std::string remoteDir = m_remotePath;
    if (!remoteDir.empty() && remoteDir.back() != '/') remoteDir += '/';

    // 一次 sftp 会话里发多条 put —— 认证只做一次，比逐个文件起进程快得多，
    // 多选几十个文件时差别很明显。
    std::string cmds;
    for (const auto& n : names) {
        cmds += "put " + EscapeSftpPath(ToRemotePath(localDir + n)) + " " +
                EscapeSftpPath(remoteDir + WideToUtf8(n)) + "\n";
    }

    std::wstring what = (names.size() == 1)
                            ? names[0]
                            : (std::to_wstring(names.size()) + L" 个文件");
    SetStatus(L"正在上传 " + what + L" ...");
    LogLine(L"批量上传 %d 个文件：\n%S", (int)names.size(), cmds.c_str());

    Session sess = m_session;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;
    int skipped = skippedDirs;

    std::thread([hwnd, alive, sess, cmds, what, skipped]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::Transfer;

        std::string output;
        std::wstring err;
        t->ok = RunSftpBatch(sess, cmds, output, &err, 10 * 60 * 1000);
        if (t->ok) {
            t->message = L"上传完成：" + what;
            if (skipped > 0) {
                t->message += L"（跳过 " + std::to_wstring(skipped) + L" 个目录）";
            }
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
    std::vector<int> sel = SelRemoteItems();
    if (sel.empty()) {
        SetStatus(L"请先在右侧选中要下载的文件（按住 Ctrl / Shift 可以多选）。", true);
        return;
    }

    // 收集待下载的文件；目录（含 ".."）跳过
    std::vector<const SftpEntry*> files;
    int skippedDirs = 0;
    for (int idx : sel) {
        if (idx < 0 || idx >= (int)m_remoteItems.size()) continue;
        const SftpEntry& e = m_remoteItems[(size_t)idx];
        if (e.isDir) { ++skippedDirs; continue; }
        files.push_back(&e);
    }
    if (files.empty()) {
        SetStatus(L"选中的都是目录。暂不支持下载整个目录，请进入目录后选文件。", true);
        return;
    }
    if (m_busy.exchange(true)) return;
    UpdateButtons();

    std::wstring localDir = m_localPath;
    if (!localDir.empty() && localDir.back() != L'\\') localDir += L'\\';

    // 同样一次会话发多条 get
    std::string cmds;
    std::vector<std::wstring> names;
    for (const SftpEntry* e : files) {
        names.push_back(e->name);
        cmds += "get " + EscapeSftpPath(e->path) + " " +
                EscapeSftpPath(ToRemotePath(localDir + e->name)) + "\n";
    }

    std::wstring what = (names.size() == 1)
                            ? names[0]
                            : (std::to_wstring(names.size()) + L" 个文件");
    SetStatus(L"正在下载 " + what + L" ...");
    LogLine(L"批量下载 %d 个文件：\n%S", (int)names.size(), cmds.c_str());

    Session sess = m_session;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;
    int skipped = skippedDirs;

    std::thread([hwnd, alive, sess, cmds, what, skipped]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::Transfer;

        std::string output;
        std::wstring err;
        t->ok = RunSftpBatch(sess, cmds, output, &err, 10 * 60 * 1000);
        if (t->ok) {
            t->message = L"下载完成：" + what;
            if (skipped > 0) {
                t->message += L"（跳过 " + std::to_wstring(skipped) + L" 个目录）";
            }
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
    std::vector<int> sel = SelRemoteItems();
    if (sel.empty()) {
        SetStatus(L"请先在右侧选中要删除的项目（按住 Ctrl / Shift 可以多选）。", true);
        return;
    }

    // 收集待删除项。".." 是我们自己塞进去做导航的，磁盘上并不存在 ——
    // 放它过去会把 rmdir 打在父目录上，所以直接跳过。
    std::vector<const SftpEntry*> items;
    for (int idx : sel) {
        if (idx < 0 || idx >= (int)m_remoteItems.size()) continue;
        const SftpEntry& e = m_remoteItems[(size_t)idx];
        if (e.name == L"..") continue;
        items.push_back(&e);
    }
    if (items.empty()) {
        SetStatus(L"「..」是用来返回上级的，不是真实条目，不能删除。", true);
        return;
    }

    std::wstring msg;
    if (items.size() == 1) {
        const SftpEntry* e = items[0];
        msg = L"确定要删除远程" + std::wstring(e->isDir ? L"目录" : L"文件") +
              L"「" + e->name + L"」吗？\n\n" + Utf8ToWide(e->path) + L"\n\n此操作不可撤销。";
    } else {
        msg = L"确定要删除选中的 " + std::to_wstring(items.size()) + L" 个项目吗？\n\n";
        for (size_t i = 0; i < items.size() && i < 10; ++i) {
            msg += L"  " + items[i]->name + L"\n";
        }
        if (items.size() > 10) msg += L"  ...（其余 " + std::to_wstring(items.size() - 10) + L" 项）\n";
        msg += L"\n此操作不可撤销。";
    }
    if (MessageBoxW(m_hwnd, msg.c_str(), L"删除远程项目",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (m_busy.exchange(true)) return;
    UpdateButtons();
    SetStatus(L"正在删除...");

    // 一次会话里发多条 rm / rmdir
    std::string cmds;
    for (const SftpEntry* e : items) {
        cmds += std::string(e->isDir ? "rmdir " : "rm ") + EscapeSftpPath(e->path) + "\n";
    }
    size_t count = items.size();

    Session sess = m_session;
    HWND hwnd = m_hwnd;
    auto alive = m_alive;

    std::thread([hwnd, alive, sess, cmds, count]() {
        auto* t = new SftpTask();
        t->kind = SftpTask::Simple;

        std::string output;
        std::wstring err;
        t->ok = RunSftpBatch(sess, cmds, output, &err);
        if (t->ok) {
            t->message = (count == 1) ? L"已删除。"
                                      : (L"已删除 " + std::to_wstring(count) + L" 项。");
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
                    LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED |
                    LBS_HASSTRINGS | LBS_EXTENDEDSEL,
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
        m_btnRefresh   = mkBtn(IDC_SFTP_REFRESH, L"刷新远程");
        m_btnClose     = mkBtn(IDC_SFTP_CLOSE, L"关闭");
        m_btnOpenLocal = mkBtn(IDC_SFTP_OPEN_LOCAL, L"选择本地目录...");
        m_btnRefreshLocal = mkBtn(IDC_SFTP_REFRESH_LOCAL, L"刷新本地");

        // 两侧各起一个标签，路径取自当前值
        m_localTabs.clear();
        m_remoteTabs.clear();
        m_localTabs.resize(1);
        m_remoteTabs.resize(1);
        m_localActive = 0;
        m_remoteActive = 0;
        m_localTabs[0].localPath = m_localPath;
        m_remoteTabs[0].remotePath = m_remotePath;

        Layout();
        RefreshLocal();
        RefreshRemote();
        UpdateButtons();
        return 0;
    }

    case WM_SIZE:
        Layout();
        return 0;

    // ---- 标签栏的鼠标交互 --------------------------------------------------
    // 标签栏是自己画的（不是控件），所以命中判定和 hover 跟踪都在这里做。
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        bool onClose = false, onPlus = false;
        int side = 0, idx = -1;

        int li = HitTestTabs(m_localTabsRect, (int)m_localTabs.size(), x, y, &onClose, &onPlus);
        if (li >= 0 || onPlus) { side = 1; idx = li; }
        else {
            bool c2 = false, p2 = false;
            int ri = HitTestTabs(m_remoteTabsRect, (int)m_remoteTabs.size(), x, y, &c2, &p2);
            if (ri >= 0 || p2) { side = 2; idx = ri; onClose = c2; onPlus = p2; }
        }

        bool plus = onPlus;
        if (side != m_hoverSide || idx != m_hoverTab ||
            onClose != m_hoverClose || plus != m_hoverPlus) {
            m_hoverSide  = side;
            m_hoverTab   = idx;
            m_hoverClose = onClose;
            m_hoverPlus  = plus;
            RECT r1 = m_localTabsRect, r2 = m_remoteTabsRect;
            InvalidateRect(m_hwnd, &r1, FALSE);
            InvalidateRect(m_hwnd, &r2, FALSE);
        }
        // 标签栏的 hover 要自己跟踪离开，否则鼠标移走后高亮会一直留着
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
        TrackMouseEvent(&tme);
        break;   // 交给 DefWindowProc，不影响子控件
    }

    case WM_MOUSELEAVE:
        if (m_hoverSide != 0) {
            m_hoverSide = 0; m_hoverTab = -1; m_hoverClose = false; m_hoverPlus = false;
            RECT r1 = m_localTabsRect, r2 = m_remoteTabsRect;
            InvalidateRect(m_hwnd, &r1, FALSE);
            InvalidateRect(m_hwnd, &r2, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        bool onClose = false, onPlus = false;

        int li = HitTestTabs(m_localTabsRect, (int)m_localTabs.size(), x, y, &onClose, &onPlus);
        if (li >= 0 || onPlus) {
            if (onPlus)                          AddLocalTab();
            else if (onClose && li == m_localActive) CloseLocalTab(li);
            else if (!onClose)                   SwitchLocalTab(li);
            return 0;
        }

        bool c2 = false, p2 = false;
        int ri = HitTestTabs(m_remoteTabsRect, (int)m_remoteTabs.size(), x, y, &c2, &p2);
        if (ri >= 0 || p2) {
            if (p2)                          AddRemoteTab();
            else if (c2 && ri == m_remoteActive) CloseRemoteTab(ri);
            else if (!c2)                    SwitchRemoteTab(ri);
            return 0;
        }
        break;
    }

    // 中键点标签 = 关闭它。这是浏览器/编辑器的通行习惯，
    // 比去够那个小小的 × 顺手得多；任意标签都能关，不只是激活的那个。
    case WM_MBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        bool onClose = false, onPlus = false;

        int li = HitTestTabs(m_localTabsRect, (int)m_localTabs.size(), x, y, &onClose, &onPlus);
        if (li >= 0) { CloseLocalTab(li); return 0; }

        bool c2 = false, p2 = false;
        int ri = HitTestTabs(m_remoteTabsRect, (int)m_remoteTabs.size(), x, y, &c2, &p2);
        if (ri >= 0) { CloseRemoteTab(ri); return 0; }
        break;
    }

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
            else if (code == LBN_SELCHANGE) ReportSelection();
            return 0;
        case IDC_SFTP_REMOTE_LIST:
            if (code == LBN_DBLCLK) EnterRemote(SelRemote());
            else if (code == LBN_SELCHANGE) ReportSelection();
            return 0;
        case IDC_SFTP_UP:        DoUpload();       return 0;
        case IDC_SFTP_DOWN:      DoDownload();     return 0;
        case IDC_SFTP_MKDIR:     DoMkdir();        return 0;
        case IDC_SFTP_DELETE:    DoDeleteRemote(); return 0;
        case IDC_SFTP_REFRESH:       RefreshRemote(); return 0;
        case IDC_SFTP_REFRESH_LOCAL: RefreshLocal();  return 0;
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
