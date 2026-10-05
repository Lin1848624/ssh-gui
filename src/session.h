// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  session.h - 会话模型、持久化、ssh/sftp 命令行构造
// ===========================================================================
#pragma once

#include "common.h"

// ---------------------------------------------------------------------------
//  端口转发规则
// ---------------------------------------------------------------------------
struct PortForward {
    enum Kind { Local = 0, Remote = 1, Dynamic = 2 };

    int          kind       = Local;      // -L / -R / -D
    std::wstring listenHost = L"127.0.0.1";
    int          listenPort = 0;
    std::wstring destHost;
    int          destPort   = 0;

    bool Valid() const {
        if (listenPort <= 0 || listenPort > 65535) return false;
        if (kind == Dynamic) return true;
        return !destHost.empty() && destPort > 0 && destPort <= 65535;
    }
    std::wstring Spec() const;            // 拼成 ssh 的参数值
    std::wstring Describe() const;        // 界面上显示的一行说明
    static const wchar_t* KindName(int k);
};

// ---------------------------------------------------------------------------
//  会话
// ---------------------------------------------------------------------------
struct Session {
    std::wstring name = L"新建会话";
    std::wstring host;
    int          port = 22;
    std::wstring user;

    std::wstring keyPath;                 // 私钥文件
    std::wstring password;                // 明文只驻留内存；落盘走 DPAPI
    bool         savePassword = false;

    std::wstring startupDir;              // 登录后自动 cd
    std::wstring extraArgs;               // 追加给 ssh 的原始参数
    bool         compress  = false;
    bool         verbose   = false;
    bool         keepAlive = true;
    bool         autoAcceptHostKey = false;  // 首次连接自动信任主机密钥（存在中间人风险）
    bool         autoConnect = false;        // 程序启动后自动连这个会话

    std::vector<PortForward> forwards;

    std::wstring DisplayName() const;
    std::wstring Target() const;          // user@host
    bool         Valid() const { return !host.empty(); }
};

// ---------------------------------------------------------------------------
//  会话库
// ---------------------------------------------------------------------------
class SessionStore {
public:
    std::vector<Session> items;

    void Load();
    bool Save() const;

    static std::wstring FilePath();       // %LOCALAPPDATA%\SshGui\sessions.json
    int FindByName(const std::wstring& name) const;
};

// ---------------------------------------------------------------------------
//  外部程序定位
// ---------------------------------------------------------------------------
std::wstring FindSshExe();                // 完整路径；空 = 没找到
std::wstring FindSftpExe();

// 供 SSH_ASKPASS 使用的自身路径。**必须把反斜杠换成正斜杠**：
// OpenSSH 用 posix_spawnp 启动 askpass，而它按 POSIX 规则判断"文件名里有没有 /"——
// `C:\path\x.exe` 不含正斜杠，会被当成命令名去 PATH 里找，必然失败
// （症状就是 ssh_askpass: posix_spawnp: No such file or directory）。
std::wstring AskPassPath();

// 主机密钥是否已在 known_hosts 里。用 `ssh-keygen -F` 查询，
// 能正确处理 HashKnownHosts 与非标准端口的 [host]:port 写法。
bool IsHostKeyKnown(const std::wstring& host, int port);

// 组装"让 ssh/sftp 自动填密码"所需的环境变量。
//
// 终端会话和 SFTP 批处理都必须走这一个函数 —— 之前两处各写了一份，
// 修好终端那份却漏了 SFTP，结果 SFTP 一直认证失败（Permission denied）。
// 里面有三样缺一不可：
//   SSH_ASKPASS        必须是**正斜杠**路径（OpenSSH 用 posix_spawnp 启动它）
//   SSH_GUI_ASKPASS    我们自己用来认出"被当作 askpass 调用了"的标记
//   SSH_ASKPASS_REQUIRE=force   即使有 tty 也走 askpass
std::vector<std::pair<std::wstring, std::wstring>> BuildAskPassEnv(const Session& s,
                                                                  bool autoAcceptHostKey);

// ---------------------------------------------------------------------------
//  命令行构造
// ---------------------------------------------------------------------------
std::vector<std::wstring> BuildSshArgs(const Session& s, bool forceTty);
std::vector<std::wstring> BuildSftpArgs(const Session& s);
std::vector<std::wstring> SplitCommandLine(const std::wstring& cmd);
std::wstring QuoteArg(const std::wstring& a);
std::wstring JoinArgs(const std::vector<std::wstring>& args);

// 供 ConPTY 启动子进程用：在当前环境块上追加若干变量
std::vector<wchar_t> BuildEnvironmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& extra);

// ---------------------------------------------------------------------------
//  密码保护（Windows DPAPI，绑定当前用户账户）
// ---------------------------------------------------------------------------
std::string  ProtectPassword(const std::wstring& plain);      // -> "dpapi:<base64>"
std::wstring UnprotectPassword(const std::string& stored);
