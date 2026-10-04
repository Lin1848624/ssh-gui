// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  common.h - 公共环境、DPI、字符串与调试日志
// ===========================================================================
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// CreatePseudoConsole / ResizePseudoConsole 等需要 Win10 目标版本
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
// GDI+ 的头文件用到 PROPID / byte 这些 OLE 类型；
// WIN32_LEAN_AND_MEAN 会把 ole2.h 排除掉，所以这里显式补上。
#include <objidl.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>   // ShellExecuteW
#include <commdlg.h>    // GetOpenFileNameW
#include <shlobj.h>     // SHBrowseForFolderW
#include <dwmapi.h>     // DwmSetWindowAttribute

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

// Common Controls v6，配合链接器开关 /MANIFEST:EMBED 才会真正嵌进 exe
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
//  DPI：所有逻辑尺寸都要过 S()，不要在布局里写死物理像素
// ---------------------------------------------------------------------------
extern int g_dpi;
inline int S(int v) { return MulDiv(v, g_dpi, 96); }
inline int UnS(int v) { return g_dpi ? MulDiv(v, 96, g_dpi) : v; }

// ---------------------------------------------------------------------------
//  字符串（内部统一 UTF-8 std::string，边界处转 UTF-16）
// ---------------------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);

// ---------------------------------------------------------------------------
//  调试日志：%LOCALAPPDATA%\SshGui\sshgui.log，出问题时先看这个文件
// ---------------------------------------------------------------------------
void LogInit();
void LogLine(const wchar_t* fmt, ...);
const std::wstring& LogFilePath();

// ---------------------------------------------------------------------------
//  杂项工具
// ---------------------------------------------------------------------------
std::wstring ExePath();                       // 当前 exe 的完整路径
std::wstring ExeDir();                        // 当前 exe 所在目录
std::wstring AppDataDir();                    // %LOCALAPPDATA%\SshGui（自动创建）
std::wstring TrimW(const std::wstring& s);
bool         StartsWithW(const std::wstring& s, const std::wstring& prefix);

// 文本文件读写（UTF-8，自动跳过 BOM）
bool ReadTextFileUtf8(const std::wstring& path, std::string& out);
bool WriteTextFileUtf8(const std::wstring& path, const std::string& data);

// 在资源管理器里定位文件
void RevealInExplorer(const std::wstring& path);

// 拷贝文本到剪贴板（失败返回 false）
bool CopyToClipboard(HWND owner, const std::wstring& text);
std::wstring PasteFromClipboard(HWND owner);
