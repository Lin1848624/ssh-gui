// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  terminal.h - 终端视图子窗口：把 VtTerminal 画出来，把键盘鼠标喂进去
// ===========================================================================
#pragma once

#include "common.h"
#include "conpty.h"
#include "session.h"
#include "vt.h"

#include <atomic>
#include <functional>
#include <mutex>

#define WM_APP_TERM_DIRTY  (WM_APP + 10)   // 后台线程有新输出
#define WM_APP_TERM_EXIT   (WM_APP + 11)   // 远端会话结束
#define WM_APP_TERM_TITLE  (WM_APP + 12)   // 标题变化

class TerminalView {
public:
    TerminalView();
    ~TerminalView();

    TerminalView(const TerminalView&) = delete;
    TerminalView& operator=(const TerminalView&) = delete;

    bool Create(HWND parent, int id, const RECT& rc);
    HWND Hwnd() const { return m_hwnd; }

    // ---- 连接管理 ----
    bool Connect(const Session& s, std::wstring* err);
    void Disconnect();
    bool Connected() const { return m_pty.Running(); }
    bool Exited() const { return m_exited; }
    int  LastExitCode() const { return m_lastExit; }

    const Session& GetSession() const { return m_session; }
    std::wstring CurrentTitle() const;

    // ---- 回调（都在 UI 线程触发）----
    void SetOnTitle(std::function<void()> fn)    { m_onTitle = std::move(fn); }
    void SetOnClosed(std::function<void()> fn)   { m_onClosed = std::move(fn); }
    void SetOnActivate(std::function<void()> fn) { m_onActivate = std::move(fn); }
    void SetOnStatus(std::function<void()> fn)   { m_onStatus = std::move(fn); }

    // ---- 操作 ----
    void FocusTerminal();
    void SendRaw(const std::string& bytes);
    void SendUserInput(const std::string& bytes);

    void CopySelection();
    void PasteClipboard();
    void SelectAll();
    void ClearSelection();
    bool HasSelection() const { return m_hasSelection; }
    std::wstring SelectionText() const;

    void ZoomFont(int delta);
    int  FontSize() const { return m_fontPt; }
    void ResetFontSize();

    void ScrollToBottomNow() { ScrollToBottom(); }

    // 供主窗口在重排前询问期望尺寸
    int GridCols() const { return m_gridCols; }
    int GridRows() const { return m_gridRows; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void RebuildFont();
    void RecalcGrid();
    void Paint(HDC hdcTarget);

    void DrawRow(HDC hdc, int absRow, int y, int cols);
    void DrawRun(HDC hdc, int absRow, int c0, int c1, int y,
                 uint32_t fg, uint32_t bg, uint16_t attr);
    void DrawScrollbar(HDC hdc, int W, int H);
    void DrawCursor(HDC hdc);
    void ResolveCell(const Cell& cell, bool selected,
                     uint32_t& fg, uint32_t& bg, uint16_t& attr) const;

    void RequestRepaint();
    void ScrollToBottom();
    void ScrollLines(int delta);
    int  MaxViewOffset() const;

    void OnPtyOutput(const char* data, size_t len);
    void OnPtyExit(DWORD code);

    bool TranslateKey(UINT vk, bool ctrl, bool alt, bool shift, std::string& out);
    void SendKeyBytes(const std::string& s);
    void SendMouseEvent(int btn, int px, int py, bool press, bool wheel, int wheelDelta);
    bool MouseReportingActive() const;
    int  CurrentMouseMode() const;

    bool IsSelected(int absRow, int col) const;
    void HitTest(int px, int py, int& absRow, int& col) const;

    void CopyAndClearSelection();
    void UpdateCursorBlinkTimer();

    // ---- 数据 ----
    HWND         m_hwnd = nullptr;
    HWND         m_parent = nullptr;
    int          m_id = 0;
    VtTerminal   m_vt;
    PtyProcess   m_pty;
    Session      m_session;

    mutable std::mutex m_vtMutex;              // 保护 m_vt（读线程会 Feed）
    std::atomic<bool>  m_repaintPosted{false};
    std::atomic<bool>  m_exited{false};
    std::atomic<int>   m_lastExit{0};
    int                m_lastSbCount = 0;      // UI 线程记录，用于滚动跟随
    wchar_t            m_highSurrogate = 0;    // WM_CHAR 代理对组装

    // 字体与网格
    HFONT m_hFont = nullptr;
    HFONT m_hFontBold = nullptr;
    int   m_fontPt = 11;
    int   m_baseFontPt = 11;
    int   m_cellW = 8;
    int   m_cellH = 17;
    int   m_gridCols = 80;
    int   m_gridRows = 24;

    // 布局
    int m_padX = 5;
    int m_padY = 5;
    int m_scrollbarW = 10;

    // 视口
    int m_viewOffset = 0;

    // 选区（绝对行号）
    bool m_selecting = false;
    bool m_hasSelection = false;
    int  m_selAnchorRow = 0, m_selAnchorCol = 0;
    int  m_selRow1 = 0, m_selCol1 = 0;
    int  m_selRow2 = 0, m_selCol2 = 0;

    // 光标闪烁
    bool     m_cursorOn = true;
    UINT_PTR m_blinkTimer = 0;
    ULONGLONG m_lastInputTick = 0;

    bool m_focused = false;

    std::function<void()> m_onTitle;
    std::function<void()> m_onClosed;
    std::function<void()> m_onActivate;
    std::function<void()> m_onStatus;
};
