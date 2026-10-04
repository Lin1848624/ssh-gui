// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  fake_sftp.cpp - 端到端测试用的"假 sftp"
//
//  读 stdin 的批处理命令，按 OpenSSH sftp 客户端的实际输出格式回话，
//  用来验证 SshGui 的 SFTP 面板（含 ls -l 解析）。
//  编译成 sftp.exe 放进 PATH 最前面的目录即可顶替真 sftp。
// ===========================================================================
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static HANDLE g_out = INVALID_HANDLE_VALUE;

static void Out(const std::string& s) {
    if (g_out == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(g_out, s.data(), (DWORD)s.size(), &w, nullptr);
}

static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// 去掉 sftp 命令里的引号
static std::string Unquote(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c != '"') out += c;
    }
    return out;
}

struct Entry {
    const char* perm;
    unsigned    links;
    const char* owner;
    const char* group;
    unsigned long long size;
    const char* mon;
    const char* day;
    const char* time;
    const char* name;
};

// 模拟的目录内容
static std::vector<Entry> ListingFor(const std::string& path) {
    std::vector<Entry> v;
    if (path.find("docs") != std::string::npos) {
        v.push_back({ "drwxr-xr-x", 2, "test", "test", 4096, "Jan", "10", "08:15", "api" });
        v.push_back({ "-rw-r--r--", 1, "test", "test", 45120, "Feb", "3", "2024", "manual.pdf" });
        v.push_back({ "-rw-r--r--", 1, "test", "test", 812, "Mar", "21", "16:40", "notes.md" });
        return v;
    }
    if (path.find("api") != std::string::npos) {
        v.push_back({ "-rw-r--r--", 1, "test", "test", 2048, "Apr", "1", "10:02", "openapi.yaml" });
        return v;
    }

    v.push_back({ "drwx------", 3, "test", "test", 4096, "Jan", "1", "12:00", ".ssh" });
    v.push_back({ "drwxr-xr-x", 2, "test", "test", 4096, "Jan", "10", "08:15", "docs" });
    v.push_back({ "-rw-------", 1, "test", "test", 1675, "Jan", "1", "12:00", ".bashrc" });
    v.push_back({ "-rw-r--r--", 1, "test", "test", 1234, "Mar", "15", "09:30", "readme.txt" });
    v.push_back({ "-rwxr-xr-x", 1, "test", "test", 20480, "Feb", "28", "2024", "deploy.sh" });
    v.push_back({ "lrwxrwxrwx", 1, "test", "test", 11, "Jan", "1", "12:00", "latest -> docs/api" });
    v.push_back({ "-rw-r--r--", 1, "test", "test", 33554432ull, "Dec", "31", "2024", "backup.tar.gz" });
    return v;
}

// 由环境变量控制的模拟开关：
//   FAKE_SFTP_STYLE=toybox  用 Android/toybox 的 ISO 日期写法（7 个字段）
//   FAKE_SFTP_NO_REALPATH=1 让 realpath 命令失败（老服务器没有这个 sftp 命令）
static bool g_toyboxStyle = false;
static bool g_noRealpath = false;
static std::string g_cwd = "/home/test";   // cd 之后 pwd 要报的当前目录

static void EmitListing(const std::string& path) {
    for (const auto& e : ListingFor(path)) {
        char buf[512];
        if (g_toyboxStyle) {
            // Android toybox 风格："2024-01-01 12:00"，日期和时间各占一个字段
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "%s %3u %-8s %-8s %8llu 2024-01-01 12:00 %s\r\n",
                        e.perm, e.links, e.owner, e.group, e.size, e.name);
        } else {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "%s %3u %-8s %-8s %8llu %s %2s %s %s\r\n",
                        e.perm, e.links, e.owner, e.group, e.size, e.mon, e.day, e.time, e.name);
        }
        Out(buf);
    }
}

// 把收到的 askpass 相关环境变量落到文件，供测试脚本核对。
// 走文件而不是 stdout：stdout 会被 SFTP 的 ls 解析器读到，混进去就成噪声了。
static void WriteAskPassProbe() {
    char probePath[1024] = {};
    if (GetEnvironmentVariableA("FAKE_SFTP_PROBE", probePath, sizeof(probePath)) == 0) return;

    char ap[1024] = {}, flag[64] = {}, pw[256] = {}, require[64] = {};
    GetEnvironmentVariableA("SSH_ASKPASS", ap, sizeof(ap));
    GetEnvironmentVariableA("SSH_GUI_ASKPASS", flag, sizeof(flag));
    GetEnvironmentVariableA("SSH_GUI_PASSWORD", pw, sizeof(pw));
    GetEnvironmentVariableA("SSH_ASKPASS_REQUIRE", require, sizeof(require));

    FILE* f = nullptr;
    if (fopen_s(&f, probePath, "w") != 0 || !f) return;
    fprintf(f, "SSH_ASKPASS=%s\n", ap);
    fprintf(f, "askpass_has_backslash=%d\n", strchr(ap, '\\') ? 1 : 0);
    fprintf(f, "SSH_GUI_ASKPASS=%s\n", flag);
    fprintf(f, "SSH_ASKPASS_REQUIRE=%s\n", require);
    fprintf(f, "has_password=%d\n", pw[0] ? 1 : 0);
    fclose(f);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_out == INVALID_HANDLE_VALUE || g_out == nullptr) return 1;

    {
        char v[32] = {};
        if (GetEnvironmentVariableA("FAKE_SFTP_STYLE", v, sizeof(v)) > 0 &&
            _stricmp(v, "toybox") == 0) {
            g_toyboxStyle = true;
        }
        v[0] = 0;
        if (GetEnvironmentVariableA("FAKE_SFTP_NO_REALPATH", v, sizeof(v)) > 0 && v[0] == '1') {
            g_noRealpath = true;
        }
    }
    WriteAskPassProbe();

    (void)argc;
    (void)argv;

    // 一次读完所有命令
    std::string all;
    char buf[8192];
    DWORD got = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (ReadFile(in, buf, sizeof(buf), &got, nullptr) && got > 0) {
        all.append(buf, got);
    }

    // 逐行执行
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = Trim(all.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos));
        if (nl == std::string::npos) {
            if (line.empty()) break;
            pos = all.size() + 1;
        } else {
            pos = nl + 1;
        }
        if (line.empty()) continue;

        // sftp 的 '-' 前缀是**批处理模式**的"忽略错误"标记。客户端现在改走
        // 交互模式、不再发带前缀的命令，这里保留解析只为兼容。
        if (!line.empty() && line[0] == '-') line = Trim(line.substr(1));

        if (line.compare(0, 8, "realpath") == 0) {
            std::string p = Unquote(Trim(line.substr(8)));
            if (g_noRealpath) {
                // 交互模式下单条命令失败不会中止会话（"失败即中止"是批处理模式
                // 才有的行为），所以这里只报错，后面的 ls 照常执行
                Out("realpath \"" + p + "\": No such file or directory\r\n");
                continue;
            }
            if (p.empty() || p == ".")      Out("/home/test\r\n");
            else if (p[0] == '/')           Out(p + "\r\n");
            else                            Out("/home/test/" + p + "\r\n");
        } else if (line.compare(0, 2, "cd") == 0) {
            // 客户端现在靠 cd + pwd 拿绝对路径。真 sftp 的 cd 成功时不输出。
            std::string p = Unquote(Trim(line.substr(2)));
            if (p.empty() || p == ".") {
                // 保持原样
            } else if (p[0] == '/') {
                g_cwd = p;
            } else if (p == "..") {
                size_t slash = g_cwd.find_last_of('/');
                g_cwd = (slash == std::string::npos || slash == 0) ? std::string("/")
                                                                   : g_cwd.substr(0, slash);
            } else {
                g_cwd = (g_cwd == "/" ? std::string() : g_cwd) + "/" + p;
            }
        } else if (line.compare(0, 3, "pwd") == 0) {
            Out("Remote working directory: " + g_cwd + "\r\n");
        } else if (line.compare(0, 2, "ls") == 0) {
            std::string rest = Trim(line.substr(2));
            // 可能带 -l 等选项
            while (!rest.empty() && rest[0] == '-') {
                size_t sp = rest.find(' ');
                rest = (sp == std::string::npos) ? std::string() : Trim(rest.substr(sp + 1));
            }
            EmitListing(Unquote(rest).empty() ? g_cwd : Unquote(rest));
        } else if (line.compare(0, 3, "put") == 0) {
            Out("Uploading data to remote\r\n");
        } else if (line.compare(0, 3, "get") == 0) {
            Out("Fetching data from remote\r\n");
        } else if (line.compare(0, 5, "mkdir") == 0) {
            // 故意让已存在的目录名报错，验证"失败但不改退出码"这条路径
            // （交互模式下真 sftp 就是这样：报错归报错，最后 quit 仍返回 0）
            if (line.find("exists") != std::string::npos) {
                Out("remote mkdir \"" + Unquote(Trim(line.substr(5))) + "\": Failure\r\n");
                continue;
            }
            Out("Created directory\r\n");
        } else if (line.compare(0, 3, "rm ") == 0 || line.compare(0, 3, "rm\t") == 0) {
            Out("Removed\r\n");
        } else if (line.compare(0, 5, "rmdir") == 0) {
            Out("Removed directory\r\n");
        } else if (line.compare(0, 4, "quit") == 0 || line.compare(0, 3, "bye") == 0 ||
                   line.compare(0, 4, "exit") == 0) {
            // 交互模式下客户端会显式发 quit；真 sftp 到这里就退出了
            return 0;
        } else {
            Out("Unknown command\r\n");
        }
    }

    return 0;
}
