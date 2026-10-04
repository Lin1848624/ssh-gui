// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  dialogs.h - 会话编辑对话框
// ===========================================================================
#pragma once

#include "common.h"
#include "session.h"

// 模态会话编辑框。确定返回 true 并写回 s。
bool EditSessionDialog(HWND parent, Session& s, bool isNew);

// 单条端口转发的编辑框
bool ForwardDialog(HWND parent, PortForward& f);

// 简易文本输入框（用于"新建远程目录"这类一次性输入）
bool SimpleInputBox(HWND parent, const wchar_t* title, const wchar_t* prompt,
                    wchar_t* buf, size_t cch);
