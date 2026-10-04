// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  conpty.cpp
// ===========================================================================
#include "conpty.h"
#include "session.h"

// ---------------------------------------------------------------------------
//  ConPTY 入口点是 Win10 1809 才有的，动态取，找不到就明确报错
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT(WINAPI* PFN_ResizePseudoConsole)(HPCON, COORD);
typedef VOID(WINAPI* PFN_ClosePseudoConsole)(HPCON);

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

namespace {

struct ConPtyApi {
    PFN_CreatePseudoConsole Create = nullptr;
    PFN_ResizePseudoConsole Resize = nullptr;
    PFN_ClosePseudoConsole  Close  = nullptr;
    bool ok = false;

    ConPtyApi() {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        if (!k) return;
        Create = (PFN_CreatePseudoConsole)GetProcAddress(k, "CreatePseudoConsole");
        Resize = (PFN_ResizePseudoConsole)GetProcAddress(k, "ResizePseudoConsole");
        Close  = (PFN_ClosePseudoConsole)GetProcAddress(k, "ClosePseudoConsole");
        ok = (Create && Resize && Close);
    }
};

ConPtyApi& Api() {
    static ConPtyApi api;
    return api;
}

} // namespace

bool PtyProcess::Available() { return Api().ok; }

// ---------------------------------------------------------------------------
PtyProcess::~PtyProcess() {
    Stop();
}

void PtyProcess::CloseHandles() {
    if (m_inWrite)  { CloseHandle(m_inWrite);  m_inWrite  = nullptr; }
    if (m_outRead)  { CloseHandle(m_outRead);  m_outRead  = nullptr; }
    if (m_hThread)  { CloseHandle(m_hThread);  m_hThread  = nullptr; }
    if (m_hProcess) { CloseHandle(m_hProcess); m_hProcess = nullptr; }
    if (m_hJob)     { CloseHandle(m_hJob);     m_hJob     = nullptr; }
}

bool PtyProcess::Start(const std::wstring& exe,
                       const std::wstring& rawArgs,
                       const std::vector<std::pair<std::wstring, std::wstring>>& envExtra,
                       const std::wstring& cwd,
                       int cols, int rows,
                       std::wstring* err) {
    auto Fail = [&](const wchar_t* what) {
        DWORD e = GetLastError();
        if (err) {
            wchar_t buf[512];
            _snwprintf_s(buf, 512, _TRUNCATE, L"%s (错误码 %lu)", what, (unsigned long)e);
            *err = buf;
        }
        LogLine(L"PtyProcess::Start 失败: %s err=%lu", what, (unsigned long)e);
        CloseHandles();
        if (m_hpc) { Api().Close(m_hpc); m_hpc = nullptr; }
        return false;
    };

    if (!Api().ok) {
        if (err) *err = L"当前系统不支持 ConPTY（需要 Windows 10 1809 或更高版本）";
        return false;
    }
    if (exe.empty()) {
        if (err) *err = L"可执行文件路径为空";
        return false;
    }

    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;

    HANDLE inRead = nullptr, inWrite = nullptr;
    HANDLE outRead = nullptr, outWrite = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return Fail(L"创建输入管道失败");
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        CloseHandle(inRead); CloseHandle(inWrite);
        return Fail(L"创建输出管道失败");
    }

    COORD size = { (SHORT)cols, (SHORT)rows };
    HRESULT hr = Api().Create(size, inRead, outWrite, 0, &m_hpc);
    if (FAILED(hr)) {
        CloseHandle(inRead);  CloseHandle(inWrite);
        CloseHandle(outRead); CloseHandle(outWrite);
        if (err) {
            wchar_t buf[256];
            _snwprintf_s(buf, 256, _TRUNCATE, L"CreatePseudoConsole 失败 (HRESULT 0x%08X)", (unsigned)hr);
            *err = buf;
        }
        return false;
    }

    // ConPTY 已持有这两个端，进程侧必须关掉，否则管道不会 EOF
    CloseHandle(inRead);
    CloseHandle(outWrite);

    m_inWrite = inWrite;
    m_outRead = outRead;

    // ---- 组装命令行 ----
    std::wstring cmdline = QuoteArg(exe);
    if (!rawArgs.empty()) cmdline += L" " + rawArgs;

    std::vector<wchar_t> envBlock = BuildEnvironmentBlock(envExtra);

    // ---- 进程属性：把伪终端挂上去 ----
    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    std::vector<BYTE> attrBuf(attrBytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrBytes)) {
        if (err) *err = L"InitializeProcThreadAttributeList 失败";
        CloseHandles();
        Api().Close(m_hpc); m_hpc = nullptr;
        return false;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   m_hpc, sizeof(m_hpc), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        if (err) *err = L"UpdateProcThreadAttribute(PSEUDOCONSOLE) 失败";
        CloseHandles();
        Api().Close(m_hpc); m_hpc = nullptr;
        return false;
    }

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;

    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(L'\0');

    // 作业对象：Stop 时能连同子孙进程（启动器 -> ssh）一起收掉
    m_hJob = CreateJobObjectW(nullptr, nullptr);
    if (m_hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            LogLine(L"SetInformationJobObject 失败 err=%lu", (unsigned long)GetLastError());
        }
    }

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (m_hJob) flags |= CREATE_SUSPENDED;   // 先挂起，塞进作业对象后再放行

    BOOL created = CreateProcessW(
        exe.c_str(),
        mutableCmd.data(),
        nullptr, nullptr,
        FALSE,
        flags,
        envBlock.data(),
        cwd.empty() ? nullptr : cwd.c_str(),
        &si.StartupInfo,
        &pi);

    DeleteProcThreadAttributeList(attrs);

    if (!created) {
        DWORD e = GetLastError();
        if (err) {
            wchar_t buf[512];
            _snwprintf_s(buf, 512, _TRUNCATE, L"启动进程失败：%s (错误码 %lu)", exe.c_str(), (unsigned long)e);
            *err = buf;
        }
        LogLine(L"CreateProcessW 失败 err=%lu cmd=%s", (unsigned long)e, cmdline.c_str());
        CloseHandles();
        Api().Close(m_hpc); m_hpc = nullptr;
        return false;
    }

    if (m_hJob) {
        if (!AssignProcessToJobObject(m_hJob, pi.hProcess)) {
            LogLine(L"AssignProcessToJobObject 失败 err=%lu", (unsigned long)GetLastError());
        }
        ResumeThread(pi.hThread);
    }

    m_hProcess = pi.hProcess;
    m_hThread  = pi.hThread;
    m_pid      = pi.dwProcessId;
    m_exitCode = 0;
    m_stopping = false;
    m_wqStop   = false;
    m_running  = true;

    LogLine(L"PtyProcess 启动 pid=%lu cols=%d rows=%d cmd=%s",
            (unsigned long)m_pid, cols, rows, cmdline.c_str());

    m_readThread  = std::thread(&PtyProcess::ReadLoop, this);
    m_writeThread = std::thread(&PtyProcess::WriteLoop, this);
    return true;
}

// ---------------------------------------------------------------------------
void PtyProcess::ReadLoop() {
    std::vector<char> buf(64 * 1024);
    int idlePolls = 0;

    // 不能用阻塞式 ReadFile 死等：子进程退出后 ConPTY 并不会立刻关掉输出管道，
    // 读线程会一直挂在那里，界面上就永远看不到"会话已结束"。
    // 改成先 PeekNamedPipe 探一下，没数据时顺带看进程是否已经退出，
    // 退出且静默了一小会儿才收摊 —— 这样既不丢结尾的输出，也能及时收尾。
    while (m_running.load()) {
        DWORD avail = 0;
        if (!PeekNamedPipe(m_outRead, nullptr, 0, nullptr, &avail, nullptr)) {
            LogLine(L"PtyProcess: PeekNamedPipe 失败 err=%lu", (unsigned long)GetLastError());
            break;
        }

        if (avail == 0) {
            bool procGone = m_hProcess && (WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0);
            if (procGone) {
                if (++idlePolls >= 12) break;      // 约 300ms 静默后收尾
            } else {
                idlePolls = 0;
            }
            Sleep(25);
            continue;
        }

        idlePolls = 0;
        DWORD want = (DWORD)((avail < buf.size()) ? avail : buf.size());
        DWORD got = 0;
        if (!ReadFile(m_outRead, buf.data(), want, &got, nullptr) || got == 0) {
            LogLine(L"PtyProcess: ReadFile 结束 err=%lu", (unsigned long)GetLastError());
            break;
        }

        std::function<void(const char*, size_t)> cb;
        {
            std::lock_guard<std::mutex> lk(m_cbMutex);
            cb = m_onOutput;
        }
        if (cb) cb(buf.data(), (size_t)got);
    }

    m_running = false;

    // 通知写线程别再等了
    {
        std::lock_guard<std::mutex> lk(m_wqMutex);
        m_wqStop = true;
    }
    m_wqCv.notify_all();

    // 进程退出码
    DWORD code = 0;
    if (m_hProcess) {
        WaitForSingleObject(m_hProcess, 3000);
        GetExitCodeProcess(m_hProcess, &code);
    }
    m_exitCode = code;
    LogLine(L"PtyProcess 会话结束 exit=%lu", (unsigned long)code);

    std::function<void(DWORD)> cb;
    {
        std::lock_guard<std::mutex> lk(m_cbMutex);
        cb = m_onExit;
    }
    if (cb) cb(code);
}

void PtyProcess::WriteLoop() {
    for (;;) {
        std::vector<char> chunk;
        {
            std::unique_lock<std::mutex> lk(m_wqMutex);
            m_wqCv.wait(lk, [this] { return m_wqStop || !m_wq.empty(); });
            if (m_wq.empty()) {
                if (m_wqStop) return;
                continue;
            }
            chunk.swap(m_wq);
        }

        size_t off = 0;
        while (off < chunk.size()) {
            DWORD wrote = 0;
            if (!m_inWrite) return;
            if (!WriteFile(m_inWrite, chunk.data() + off, (DWORD)(chunk.size() - off), &wrote, nullptr) || wrote == 0) {
                LogLine(L"PtyProcess 写入失败 err=%lu", (unsigned long)GetLastError());
                return;
            }
            off += wrote;
        }
    }
}

bool PtyProcess::Write(const char* data, size_t len) {
    if (!m_inWrite || len == 0) return false;
    {
        std::lock_guard<std::mutex> lk(m_wqMutex);
        if (m_wqStop) return false;
        // 防止极端情况下输入队列无限增长
        if (m_wq.size() > 4u * 1024 * 1024) return false;
        m_wq.insert(m_wq.end(), data, data + len);
    }
    m_wqCv.notify_one();
    return true;
}

void PtyProcess::Resize(int cols, int rows) {
    if (!m_hpc || !Api().ok) return;
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    COORD size = { (SHORT)cols, (SHORT)rows };
    Api().Resize(m_hpc, size);
}

// ---------------------------------------------------------------------------
void PtyProcess::Stop() {
    if (!m_hpc && !m_hProcess && !m_readThread.joinable() && !m_writeThread.joinable()) return;

    LogLine(L"PtyProcess::Stop 开始 pid=%lu", (unsigned long)m_pid);
    m_stopping = true;
    m_running  = false;

    // 1) 关掉输入管道 -> 子进程收到 EOF，通常自行退出
    if (m_inWrite) {
        CloseHandle(m_inWrite);
        m_inWrite = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(m_wqMutex);
        m_wqStop = true;
        m_wq.clear();
    }
    m_wqCv.notify_all();

    // 2) 等进程退出，超时强杀整棵进程树
    if (m_hProcess) {
        DWORD w = WaitForSingleObject(m_hProcess, 1500);
        if (w == WAIT_TIMEOUT) {
            LogLine(L"PtyProcess: 进程未自行退出，强制结束");
            if (m_hJob) TerminateJobObject(m_hJob, 1);
            else        TerminateProcess(m_hProcess, 1);
            WaitForSingleObject(m_hProcess, 1000);
        }
    }

    // 3) 关伪终端：会让输出端 EOF，读线程自然退出
    if (m_hpc) {
        Api().Close(m_hpc);
        m_hpc = nullptr;
    }

    // 4) 回收线程
    if (m_readThread.joinable()) {
        // 若 ReadFile 仍阻塞（管道端未关），取消它的同步 IO
        if (m_stopping && m_outRead) {
            CancelSynchronousIo(m_readThread.native_handle());
        }
        m_readThread.join();
    }
    if (m_writeThread.joinable()) m_writeThread.join();

    CloseHandles();

    DWORD code = m_exitCode;
    m_pid = 0;
    LogLine(L"PtyProcess::Stop 完成 exit=%lu", (unsigned long)code);
}
