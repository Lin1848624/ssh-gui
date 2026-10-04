// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  sftppanel.h - SFTP 文件传输面板
// ===========================================================================
#pragma once

#include "common.h"
#include "session.h"

// 为一个会话打开文件传输窗口（同一会话重复调用会激活已有窗口）
void OpenSftpWindow(HWND parent, const Session& s);

// 程序退出时关闭全部
void CloseAllSftpWindows();

// ---------------------------------------------------------------------------
//  底层：调用 sftp.exe 的批处理模式
// ---------------------------------------------------------------------------
struct SftpEntry {
    std::wstring name;        // 显示名
    std::string  path;        // 完整远程路径（UTF-8）
    bool         isDir  = false;
    bool         isLink = false;
    uint64_t     size   = 0;
    std::wstring date;
};

// 执行一组 sftp 批处理命令，返回合并后的 stdout
bool RunSftpBatch(const Session& s,
                  const std::string& commands,
                  std::string& output,
                  std::wstring* err,
                  DWORD timeoutMs = 30000);

// 列远程目录；返回绝对路径（realpath 结果）
bool SftpListDir(const Session& s, const std::string& path,
                 std::vector<SftpEntry>& out, std::string& absPath, std::wstring* err);
