// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  conpty.h - 伪终端进程封装（Windows ConPTY）
//  把 ssh.exe / sftp.exe 跑在一个真终端里，界面侧拿到的是原始的 VT 字节流。
// ===========================================================================
#pragma once

#include "common.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class PtyProcess {
public:
    PtyProcess() = default;
    ~PtyProcess();

    PtyProcess(const PtyProcess&) = delete;
    PtyProcess& operator=(const PtyProcess&) = delete;

    // 系统是否提供 ConPTY（Win10 1809+）
    static bool Available();

    // 启动子进程。rawArgs 是已拼好引号的参数串（调用方负责引号规则），
    // cwd 传空串表示继承当前目录。
    bool Start(const std::wstring& exe,
               const std::wstring& rawArgs,
               const std::vector<std::pair<std::wstring, std::wstring>>& envExtra,
               const std::wstring& cwd,
               int cols, int rows,
               std::wstring* err);

    // 停止并回收：先关输入发 EOF，等待退出，超时强杀，最后 join 线程
    void Stop();

    bool Write(const char* data, size_t len);
    void Resize(int cols, int rows);

    bool  Running() const { return m_running.load(); }
    DWORD ExitCode() const { return m_exitCode; }
    DWORD Pid() const { return m_pid; }

    // 回调可能在读线程里被取用，赋值必须加锁
    void SetOnOutput(std::function<void(const char*, size_t)> fn) {
        std::lock_guard<std::mutex> lk(m_cbMutex);
        m_onOutput = std::move(fn);
    }
    void SetOnExit(std::function<void(DWORD)> fn) {
        std::lock_guard<std::mutex> lk(m_cbMutex);
        m_onExit = std::move(fn);
    }

private:
    void ReadLoop();
    void WriteLoop();

    HPCON  m_hpc      = nullptr;
    HANDLE m_inWrite  = nullptr;   // 我们 -> 伪终端输入
    HANDLE m_outRead  = nullptr;   // 伪终端输出 -> 我们
    HANDLE m_hProcess = nullptr;
    HANDLE m_hThread  = nullptr;
    HANDLE m_hJob     = nullptr;   // 作业对象：Stop 时连同子孙进程一起收掉
    DWORD  m_pid      = 0;
    DWORD  m_exitCode = 0;

    std::thread       m_readThread;
    std::thread       m_writeThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};

    std::mutex              m_wqMutex;
    std::condition_variable m_wqCv;
    std::vector<char>       m_wq;
    bool                    m_wqStop = false;

    std::function<void(const char*, size_t)> m_onOutput;
    std::function<void(DWORD)>               m_onExit;
    std::mutex                               m_cbMutex;   // 保护上面两个回调的读写

    void CloseHandles();
};
