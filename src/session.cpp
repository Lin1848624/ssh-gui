// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  session.cpp
// ===========================================================================
#include "session.h"
#include "json.h"

#include <dpapi.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

// ---------------------------------------------------------------------------
//  PortForward
// ---------------------------------------------------------------------------
const wchar_t* PortForward::KindName(int k) {
    switch (k) {
    case Remote:  return L"远程 -R";
    case Dynamic: return L"动态 -D";
    default:      return L"本地 -L";
    }
}

std::wstring PortForward::Spec() const {
    std::wstring out;
    if (!listenHost.empty() && listenHost != L"*") out += listenHost + L":";
    out += std::to_wstring(listenPort);
    if (kind != Dynamic) {
        out += L":" + destHost + L":" + std::to_wstring(destPort);
    }
    return out;
}

std::wstring PortForward::Describe() const {
    std::wstring out = KindName(kind);
    out += L"  ";
    if (kind == Dynamic) {
        out += (listenHost.empty() ? L"127.0.0.1" : listenHost) + L":" + std::to_wstring(listenPort) + L"  (SOCKS5)";
    } else {
        out += (listenHost.empty() ? L"127.0.0.1" : listenHost) + L":" + std::to_wstring(listenPort);
        out += L"  ->  " + destHost + L":" + std::to_wstring(destPort);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Session
// ---------------------------------------------------------------------------
std::wstring Session::DisplayName() const {
    if (!name.empty()) return name;
    return Target();
}

std::wstring Session::Target() const {
    if (user.empty()) return host;
    return user + L"@" + host;
}

// ---------------------------------------------------------------------------
//  DPAPI 密码保护
// ---------------------------------------------------------------------------
static std::string Base64Encode(const BYTE* data, DWORD len) {
    DWORD cch = 0;
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &cch)) return std::string();
    std::string out((size_t)cch, '\0');
    if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &cch)) return std::string();
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

static bool Base64Decode(const std::string& s, std::vector<BYTE>& out) {
    DWORD cb = 0;
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &cb, nullptr, nullptr)) return false;
    out.assign(cb, 0);
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, out.data(), &cb, nullptr, nullptr)) return false;
    out.resize(cb);
    return true;
}

std::string ProtectPassword(const std::wstring& plain) {
    if (plain.empty()) return std::string();
    std::string utf8 = WideToUtf8(plain);
    if (utf8.empty()) return std::string();

    DATA_BLOB in = {};
    in.pbData = (BYTE*)utf8.data();
    in.cbData = (DWORD)utf8.size();

    DATA_BLOB out = {};
    if (!CryptProtectData(&in, L"SshGui session password", nullptr, nullptr, nullptr, 0, &out)) {
        LogLine(L"ProtectPassword: CryptProtectData 失败, err=%lu", GetLastError());
        return std::string();
    }
    std::string b64 = Base64Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (b64.empty()) return std::string();
    return "dpapi:" + b64;
}

std::wstring UnprotectPassword(const std::string& stored) {
    if (stored.empty()) return std::wstring();
    const std::string prefix = "dpapi:";
    if (stored.compare(0, prefix.size(), prefix) != 0) return std::wstring();

    std::vector<BYTE> blob;
    if (!Base64Decode(stored.substr(prefix.size()), blob) || blob.empty()) return std::wstring();

    DATA_BLOB in = {};
    in.pbData = blob.data();
    in.cbData = (DWORD)blob.size();

    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        LogLine(L"UnprotectPassword: CryptUnprotectData 失败, err=%lu", GetLastError());
        return std::wstring();
    }
    std::wstring w = Utf8ToWide(std::string((char*)out.pbData, out.cbData));
    LocalFree(out.pbData);
    return w;
}

// ---------------------------------------------------------------------------
//  SessionStore
// ---------------------------------------------------------------------------
std::wstring SessionStore::FilePath() {
    std::wstring dir = AppDataDir();
    if (dir.empty()) return std::wstring();
    return dir + L"\\sessions.json";
}

static Json ForwardToJson(const PortForward& f) {
    Json j = Json::Obj();
    j.Set("kind", f.kind);
    j.Set("listenHost", WideToUtf8(f.listenHost));
    j.Set("listenPort", f.listenPort);
    j.Set("destHost", WideToUtf8(f.destHost));
    j.Set("destPort", f.destPort);
    return j;
}

static PortForward ForwardFromJson(const Json& j) {
    PortForward f;
    f.kind       = j.GetInt("kind", 0);
    if (f.kind < 0 || f.kind > 2) f.kind = PortForward::Local;
    f.listenHost = Utf8ToWide(j.GetStr("listenHost", "127.0.0.1"));
    if (f.listenHost.empty()) f.listenHost = L"127.0.0.1";
    f.listenPort = j.GetInt("listenPort", 0);
    f.destHost   = Utf8ToWide(j.GetStr("destHost"));
    f.destPort   = j.GetInt("destPort", 0);
    return f;
}

static Json SessionToJson(const Session& s) {
    Json j = Json::Obj();
    j.Set("name", WideToUtf8(s.name));
    j.Set("host", WideToUtf8(s.host));
    j.Set("port", s.port);
    j.Set("user", WideToUtf8(s.user));
    j.Set("keyPath", WideToUtf8(s.keyPath));
    j.Set("startupDir", WideToUtf8(s.startupDir));
    j.Set("extraArgs", WideToUtf8(s.extraArgs));
    j.Set("compress", s.compress);
    j.Set("verbose", s.verbose);
    j.Set("keepAlive", s.keepAlive);
    j.Set("autoAcceptHostKey", s.autoAcceptHostKey);
    j.Set("savePassword", s.savePassword);
    if (s.savePassword && !s.password.empty()) {
        std::string enc = ProtectPassword(s.password);
        if (!enc.empty()) j.Set("password", enc);
    }
    Json arr = Json::Arr();
    for (const auto& f : s.forwards) arr.arr.push_back(ForwardToJson(f));
    j.Set("forwards", arr);
    return j;
}

static Session SessionFromJson(const Json& j) {
    Session s;
    s.name        = Utf8ToWide(j.GetStr("name", "会话"));
    s.host        = Utf8ToWide(j.GetStr("host"));
    s.port        = j.GetInt("port", 22);
    if (s.port <= 0 || s.port > 65535) s.port = 22;
    s.user        = Utf8ToWide(j.GetStr("user"));
    s.keyPath     = Utf8ToWide(j.GetStr("keyPath"));
    s.startupDir  = Utf8ToWide(j.GetStr("startupDir"));
    s.extraArgs   = Utf8ToWide(j.GetStr("extraArgs"));
    s.compress    = j.GetBool("compress", false);
    s.verbose     = j.GetBool("verbose", false);
    s.keepAlive   = j.GetBool("keepAlive", true);
    s.autoAcceptHostKey = j.GetBool("autoAcceptHostKey", false);
    s.savePassword = j.GetBool("savePassword", false);
    if (s.savePassword) s.password = UnprotectPassword(j.GetStr("password"));
    if (const std::vector<Json>* fa = j.GetArr("forwards")) {
        for (const auto& fj : *fa) s.forwards.push_back(ForwardFromJson(fj));
    }
    return s;
}

void SessionStore::Load() {
    items.clear();
    std::wstring path = FilePath();
    if (path.empty()) return;

    std::string text;
    if (!ReadTextFileUtf8(path, text) || text.empty()) {
        LogLine(L"SessionStore::Load: 无配置文件或为空 (%s)", path.c_str());
        return;
    }

    Json root;
    std::string err;
    if (!Json::Parse(text, root, &err)) {
        LogLine(L"SessionStore::Load: JSON 解析失败 - %S", err.c_str());
        return;
    }
    const std::vector<Json>* arr = root.GetArr("sessions");
    if (!arr) {
        LogLine(L"SessionStore::Load: 缺少 sessions 数组");
        return;
    }
    for (const auto& j : *arr) items.push_back(SessionFromJson(j));
    LogLine(L"SessionStore::Load: 载入 %d 个会话", (int)items.size());
}

bool SessionStore::Save() const {
    std::wstring path = FilePath();
    if (path.empty()) return false;

    Json root = Json::Obj();
    root.Set("version", 1);
    Json arr = Json::Arr();
    for (const auto& s : items) arr.arr.push_back(SessionToJson(s));
    root.Set("sessions", arr);

    bool ok = WriteTextFileUtf8(path, root.Dump(2) + "\n");
    LogLine(L"SessionStore::Save: %s -> %s", ok ? L"成功" : L"失败", path.c_str());
    return ok;
}

int SessionStore::FindByName(const std::wstring& name) const {
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].name == name) return (int)i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
//  定位 ssh.exe / sftp.exe
// ---------------------------------------------------------------------------
static std::wstring FindTool(const wchar_t* exeName) {
    wchar_t buf[MAX_PATH * 2] = {};
    // 优先 PATH
    DWORD n = SearchPathW(nullptr, exeName, nullptr, (DWORD)(MAX_PATH * 2), buf, nullptr);
    if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf, n);

    // 退路：System32\OpenSSH
    wchar_t sys[MAX_PATH * 2] = {};
    UINT sn = GetSystemDirectoryW(sys, MAX_PATH * 2);
    if (sn > 0) {
        std::wstring p = std::wstring(sys, sn) + L"\\OpenSSH\\" + exeName;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return std::wstring();
}

std::wstring FindSshExe()  { return FindTool(L"ssh.exe"); }
std::wstring FindSftpExe() { return FindTool(L"sftp.exe"); }

// ---------------------------------------------------------------------------
//  askpass 支撑
// ---------------------------------------------------------------------------
std::wstring AskPassPath() {
    std::wstring p = ExePath();
    // 见 session.h：这里必须是正斜杠，否则 OpenSSH 的 posix_spawnp 找不到文件
    for (wchar_t& c : p) {
        if (c == L'\\') c = L'/';
    }
    return p;
}

bool IsHostKeyKnown(const std::wstring& host, int port) {
    if (host.empty()) return false;

    std::wstring keygen = FindTool(L"ssh-keygen.exe");
    if (keygen.empty()) {
        LogLine(L"IsHostKeyKnown: 没找到 ssh-keygen.exe，按未知处理");
        return false;
    }

    // known_hosts 里非标准端口的写法是 [host]:port
    std::wstring target = host;
    if (port != 22) target = L"[" + host + L"]:" + std::to_wstring(port);

    std::wstring cmdline = QuoteArg(keygen) + L" -F " + QuoteArg(target);
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(L'\0');

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE | GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(keygen.c_str(), mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!ok) {
        LogLine(L"IsHostKeyKnown: 启动 ssh-keygen 失败 err=%lu", (unsigned long)GetLastError());
        return false;
    }

    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    bool known = (code == 0);
    LogLine(L"IsHostKeyKnown: %s -> %s (退出码 %lu)",
            target.c_str(), known ? L"已知" : L"未知", (unsigned long)code);
    return known;
}

std::vector<std::pair<std::wstring, std::wstring>> BuildAskPassEnv(const Session& s,
                                                                  bool autoAcceptHostKey) {
    std::vector<std::pair<std::wstring, std::wstring>> env;
    if (s.password.empty()) return env;   // 没存密码就别开 askpass

    env.emplace_back(L"SSH_ASKPASS", AskPassPath());          // 正斜杠，见 AskPassPath 注释
    env.emplace_back(L"SSH_ASKPASS_REQUIRE", L"force");       // 有 tty 也要走 askpass
    env.emplace_back(L"SSH_GUI_ASKPASS", L"1");               // ssh 只传提示串，靠这个认自己
    env.emplace_back(L"SSH_GUI_PASSWORD", s.password);
    env.emplace_back(L"SSH_GUI_AUTO_ACCEPT", autoAcceptHostKey ? L"1" : L"0");
    env.emplace_back(L"DISPLAY", L":0");                      // 部分版本用它判断能否走 askpass
    return env;
}

// ---------------------------------------------------------------------------
//  命令行
// ---------------------------------------------------------------------------
std::vector<std::wstring> SplitCommandLine(const std::wstring& cmd) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQuote = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        wchar_t c = cmd[i];
        if (inQuote) {
            if (c == L'"') inQuote = false;
            else cur.push_back(c);
        } else if (c == L'"') {
            inQuote = true;
        } else if (c == L' ' || c == L'\t') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

std::wstring JoinArgs(const std::vector<std::wstring>& args) {
    std::wstring out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out += L' ';
        out += QuoteArg(args[i]);
    }
    return out;
}

std::vector<std::wstring> BuildSshArgs(const Session& s, bool forceTty) {
    std::vector<std::wstring> a;
    if (forceTty) a.push_back(L"-tt");
    a.push_back(L"-p");
    a.push_back(std::to_wstring(s.port));

    if (s.keepAlive) {
        a.push_back(L"-o"); a.push_back(L"ServerAliveInterval=30");
        a.push_back(L"-o"); a.push_back(L"ServerAliveCountMax=3");
        a.push_back(L"-o"); a.push_back(L"TCPKeepAlive=yes");
    }
    if (s.compress) a.push_back(L"-C");
    if (s.verbose)  a.push_back(L"-v");

    if (!s.keyPath.empty()) {
        a.push_back(L"-i"); a.push_back(s.keyPath);
        a.push_back(L"-o"); a.push_back(L"IdentitiesOnly=yes");
    }

    for (const auto& f : s.forwards) {
        if (!f.Valid()) continue;
        const wchar_t* flag = (f.kind == PortForward::Remote) ? L"-R"
                            : (f.kind == PortForward::Dynamic) ? L"-D" : L"-L";
        a.push_back(flag);
        a.push_back(f.Spec());
    }

    if (!s.extraArgs.empty()) {
        for (auto& t : SplitCommandLine(s.extraArgs)) a.push_back(t);
    }

    a.push_back(s.Target());
    return a;
}

std::vector<std::wstring> BuildSftpArgs(const Session& s) {
    std::vector<std::wstring> a;
    a.push_back(L"-o"); a.push_back(L"BatchMode=no");
    a.push_back(L"-P"); a.push_back(std::to_wstring(s.port));
    if (s.keepAlive) {
        a.push_back(L"-o"); a.push_back(L"ServerAliveInterval=30");
    }
    if (s.compress) a.push_back(L"-C");
    if (!s.keyPath.empty()) {
        a.push_back(L"-i"); a.push_back(s.keyPath);
        a.push_back(L"-o"); a.push_back(L"IdentitiesOnly=yes");
    }
    a.push_back(s.Target());
    return a;
}

// ---------------------------------------------------------------------------
//  环境块
// ---------------------------------------------------------------------------
std::vector<wchar_t> BuildEnvironmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<wchar_t> block;

    // 取当前环境
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p; ) {
            size_t len = wcslen(p);
            // 跳过以 '=' 开头的特殊变量（如 =C:），CreateProcess 不接受
            if (len > 0 && p[0] != L'=') {
                block.insert(block.end(), p, p + len);
                block.push_back(L'\0');
            }
            p += len + 1;
        }
        FreeEnvironmentStringsW(env);
    }

    for (const auto& kv : extra) {
        std::wstring line = kv.first + L"=" + kv.second;
        block.insert(block.end(), line.begin(), line.end());
        block.push_back(L'\0');
    }

    block.push_back(L'\0');
    return block;
}
