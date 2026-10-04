// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  common.cpp
// ===========================================================================
#include "common.h"

#include <cstdarg>
#include <shlobj.h>

int g_dpi = 96;

// ---------------------------------------------------------------------------
//  字符串
// ---------------------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
//  日志
// ---------------------------------------------------------------------------
static std::wstring g_logPath;

static void LogTimestamp(wchar_t* buf, size_t cch) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(buf, cch, _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void LogInit() {
    std::wstring dir = AppDataDir();
    if (dir.empty()) return;
    g_logPath = dir + L"\\sshgui.log";

    // 超过 2MB 就轮转一次，避免无限增长
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart > 2ull * 1024 * 1024) {
            std::wstring bak = g_logPath + L".1";
            DeleteFileW(bak.c_str());
            MoveFileW(g_logPath.c_str(), bak.c_str());
        }
    }
    LogLine(L"==== SshGui start (pid=%lu) ====", (unsigned long)GetCurrentProcessId());
}

void LogLine(const wchar_t* fmt, ...) {
    if (g_logPath.empty()) return;

    wchar_t body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, 2048, _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t ts[64];
    LogTimestamp(ts, 64);

    std::wstring line = std::wstring(ts) + L"  " + body + L"\r\n";
    std::string utf8 = WideToUtf8(line);

    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
}

const std::wstring& LogFilePath() { return g_logPath; }

// ---------------------------------------------------------------------------
//  路径
// ---------------------------------------------------------------------------
std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(MAX_PATH * 2));
    if (n == 0) return std::wstring();
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(0, pos);
}

std::wstring AppDataDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    // CSIDL_LOCAL_APPDATA == 0x1C
    if (FAILED(SHGetFolderPathW(nullptr, 0x001C, nullptr, 0, buf))) return std::wstring();
    std::wstring dir = std::wstring(buf) + L"\\SshGui";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring TrimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

bool StartsWithW(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------------------
//  文本文件读写
// ---------------------------------------------------------------------------
bool ReadTextFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (LONGLONG)64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }

    out.assign((size_t)sz.QuadPart, '\0');
    size_t total = 0;
    while (total < out.size()) {
        DWORD chunk = 0;
        DWORD want = (DWORD)std::min<size_t>(out.size() - total, 1u << 20);
        if (!ReadFile(h, &out[total], want, &chunk, nullptr) || chunk == 0) break;
        total += chunk;
    }
    out.resize(total);
    CloseHandle(h);

    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool WriteTextFileUtf8(const std::wstring& path, const std::string& data) {
    // 先写临时文件再替换，避免写一半断电把配置毁了
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    size_t total = 0;
    bool ok = true;
    while (total < data.size()) {
        DWORD chunk = 0;
        DWORD want = (DWORD)std::min<size_t>(data.size() - total, 1u << 20);
        if (!WriteFile(h, data.data() + total, want, &chunk, nullptr) || chunk == 0) { ok = false; break; }
        total += chunk;
    }
    FlushFileBuffers(h);
    CloseHandle(h);

    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void RevealInExplorer(const std::wstring& path) {
    std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
//  剪贴板
// ---------------------------------------------------------------------------
bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();

    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) { CloseClipboard(); return false; }
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); CloseClipboard(); return false; }
    memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(mem);

    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

std::wstring PasteFromClipboard(HWND owner) {
    std::wstring out;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return out;
    if (!OpenClipboard(owner)) return out;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            out.assign(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();

    // 终端粘贴：统一换行，去掉裸 \r
    std::wstring cleaned;
    cleaned.reserve(out.size());
    for (wchar_t c : out) {
        if (c == L'\r') continue;
        cleaned.push_back(c);
    }
    return cleaned;
}
