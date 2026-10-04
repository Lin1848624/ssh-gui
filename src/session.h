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
