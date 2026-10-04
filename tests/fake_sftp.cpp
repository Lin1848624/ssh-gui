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

static void EmitListing(const std::string& path) {
    for (const auto& e : ListingFor(path)) {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "%s %3u %-8s %-8s %8llu %s %2s %s %s\r\n",
                    e.perm, e.links, e.owner, e.group, e.size, e.mon, e.day, e.time, e.name);
        Out(buf);
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_out == INVALID_HANDLE_VALUE || g_out == nullptr) return 1;

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

        // 去掉开头的 '-'（sftp 的忽略错误前缀）
        if (line[0] == '-') line = Trim(line.substr(1));

        if (line.compare(0, 8, "realpath") == 0) {
            std::string p = Trim(line.substr(8));
            p = Unquote(p);
            if (p.empty() || p == ".")      Out("/home/test\r\n");
            else if (p[0] == '/')           Out(p + "\r\n");
            else                            Out("/home/test/" + p + "\r\n");
        } else if (line.compare(0, 2, "ls") == 0) {
            std::string rest = Trim(line.substr(2));
            // 可能带 -l 等选项
            while (!rest.empty() && rest[0] == '-') {
                size_t sp = rest.find(' ');
                rest = (sp == std::string::npos) ? std::string() : Trim(rest.substr(sp + 1));
            }
            EmitListing(Unquote(rest));
        } else if (line.compare(0, 3, "put") == 0) {
            Out("Uploading data to remote\r\n");
        } else if (line.compare(0, 3, "get") == 0) {
            Out("Fetching data from remote\r\n");
        } else if (line.compare(0, 5, "mkdir") == 0) {
            // 故意让已存在的目录名报错，验证错误路径
            if (line.find("exists") != std::string::npos) {
                Out("remote mkdir \"" + Unquote(Trim(line.substr(5))) + "\": Failure\r\n");
                return 1;
            }
            Out("Created directory\r\n");
        } else if (line.compare(0, 3, "rm ") == 0 || line.compare(0, 3, "rm\t") == 0) {
            Out("Removed\r\n");
        } else if (line.compare(0, 5, "rmdir") == 0) {
            Out("Removed directory\r\n");
        } else {
            Out("Unknown command\r\n");
        }
    }

    return 0;
}
