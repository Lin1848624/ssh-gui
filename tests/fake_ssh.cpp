// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  fake_ssh.cpp - 端到端测试用的"假 ssh"
//
//  在 ConPTY 里跑起来后输出各种 ANSI/VT 序列，并回显用户输入，
//  用来验证 SshGui 的伪终端集成、VT 解析与渲染是否正确。
//  编译成 ssh.exe 丢进一个目录，把该目录放到 PATH 最前面即可顶替真 ssh。
// ===========================================================================
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

static HANDLE g_out = INVALID_HANDLE_VALUE;

static void Out(const char* s) {
    if (g_out == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(g_out, s, (DWORD)strlen(s), &w, nullptr);
}

static void Out(const std::string& s) {
    Out(s.c_str());
}

static void OutW(const wchar_t* s) {
    // 宽字符统一转 UTF-8 再输出，模拟远端 Linux 的行为
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string u8((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &u8[0], n, nullptr, nullptr);
    Out(u8);
}

// ---------------------------------------------------------------------------
static void Banner() {
    Out("\x1b[2J\x1b[H");   // 清屏 + 光标归位

    OutW(L"\x1b[38;5;39m");
    OutW(L"  ╔══════════════════════════════════════════════════════════╗\r\n");
    OutW(L"  ║   SSH GUI  ·  端到端测试桩 (fake ssh)                    ║\r\n");
    OutW(L"  ║   如果这段话排版整齐、边框闭合，说明 VT 渲染正确         ║\r\n");
    OutW(L"  ╚══════════════════════════════════════════════════════════╝\r\n");
    OutW(L"\x1b[0m");

    OutW(L"\r\n  输入 \x1b[1;33mhelp\x1b[0m 查看可用命令。\r\n\r\n");
}

static void Prompt() {
    Out("\x1b[1;32mfake\x1b[0m\x1b[1;34m$\x1b[0m ");
}

// ---------------------------------------------------------------------------
static void ShowColors() {
    Out("\r\n  标准 16 色：\r\n  ");
    for (int i = 0; i < 16; ++i) {
        char b[32];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "\x1b[%d;%dm %2d \x1b[0m", (i < 8 ? 40 : 100) + (i % 8), (i < 8 ? 30 : 90) + (i % 8), i);
        Out(b);
    }
    Out("\r\n\r\n  256 色立方：\r\n  ");
    for (int i = 16; i < 232; ++i) {
        char b[32];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "\x1b[48;5;%dm ", i);
        Out(b);
        if ((i - 16) % 36 == 35) Out("\x1b[0m\r\n  ");
    }
    Out("\x1b[0m\r\n\r\n  灰度阶梯：\r\n  ");
    for (int i = 232; i < 256; ++i) {
        char b[32];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "\x1b[48;5;%dm ", i);
        Out(b);
    }
    Out("\x1b[0m\r\n");
}

static void ShowTrueColor() {
    Out("\r\n  24 位真彩色渐变：\r\n  ");
    for (int i = 0; i < 64; ++i) {
        int r = i * 4;
        int g = 255 - i * 4;
        int b = 128;
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\x1b[48;2;%d;%d;%dm ", r, g, b);
        Out(buf);
    }
    Out("\x1b[0m\r\n\r\n  前景渐变文字：\r\n  ");
    for (int i = 0; i < 32; ++i) {
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\x1b[38;2;%d;%d;%dm#", 255 - i * 8, i * 8, 200);
        Out(buf);
    }
    Out("\x1b[0m\r\n");
}

static void ShowCjk() {
    OutW(L"\r\n  中文：图形化 SSH 终端，端口转发，文件传输\r\n");
    OutW(L"  日文：こんにちは世界　カタカナ　漢字\r\n");
    OutW(L"  韩文：안녕하세요 세계\r\n");
    OutW(L"  全角标点：（）、。《》「」【】\r\n");
    OutW(L"  符号：★☆●○◆◇■□▲△▼▽→←↑↓\r\n");
    OutW(L"  框线：┌─┬─┐ ├─┼─┤ └─┴─┘ ╔═╦═╗ ╚═╩═╝\r\n");
    OutW(L"  对齐检查（每个汉字应正好占两格）：\r\n");
    OutW(L"  |1234567890|1234567890|1234567890|\r\n");
    OutW(L"  |中文字符宽|中文字符宽|中文字符宽|\r\n");
    OutW(L"  |ABCDEFGHIJ|ABCDEFGHIJ|ABCDEFGHIJ|\r\n");
    Out("\r\n");
}

static void ShowAttrs() {
    OutW(L"\r\n  属性：");
    Out("\x1b[1m粗体\x1b[0m ");
    Out("\x1b[2m暗淡\x1b[0m ");
    Out("\x1b[3m斜体\x1b[0m ");
    Out("\x1b[4m下划线\x1b[0m ");
    Out("\x1b[7m反显\x1b[0m ");
    Out("\x1b[9m删除线\x1b[0m ");
    Out("\x1b[1;3;4;31m粗斜下划线红\x1b[0m");
    OutW(L"\r\n\r\n  组合色：");
    Out("\x1b[1;37;44m 白字蓝底 \x1b[0m");
    Out("\x1b[1;30;103m 黑字亮黄底 \x1b[0m");
    Out("\x1b[4;38;5;208m 橙色下划线 \x1b[0m");
    OutW(L"\r\n\r\n");
}

static void ShowScrolling() {
    Out("\r\n  输出 200 行测试滚动与回看（Ctrl+Home 回到顶部，Ctrl+End 回底部）：\r\n");
    for (int i = 1; i <= 200; ++i) {
        char b[128];
        _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "  \x1b[38;5;%dm行 %3d\x1b[0m  the quick brown fox jumps over the lazy dog 0123456789\r\n",
                    16 + (i % 200), i);
        Out(b);
    }
    Out("  \x1b[1;32m--- 输出结束 ---\x1b[0m\r\n");
}

static void ShowProgress() {
    Out("\r\n  进度条（测试光标控制与重绘）：\r\n  ");
    for (int i = 0; i <= 40; ++i) {
        char b[256];
        _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "\r  \x1b[42m%s\x1b[0m%s %3d%%",
                    std::string((size_t)i, ' ').c_str(),
                    std::string((size_t)(40 - i), ' ').c_str(),
                    i * 100 / 40);
        Out(b);
        Sleep(35);
    }
    Out("\r\n");
}

static void ShowAltScreen() {
    Out("\x1b[?1049h\x1b[2J\x1b[H");   // 进入备用屏
    OutW(L"\x1b[38;5;51m");
    OutW(L"\r\n\r\n          ┌────────────────────────────────┐\r\n");
    OutW(L"          │   备用屏 (Alternate Screen)    │\r\n");
    OutW(L"          │   3 秒后自动返回主屏...        │\r\n");
    OutW(L"          └────────────────────────────────┘\r\n");
    Out("\x1b[0m");
    Sleep(3000);
    Out("\x1b[?1049l");                // 回主屏
    Out("  已返回主屏，之前的内容应当完好。\r\n");
}

static void ShowHelp() {
    OutW(L"\r\n  可用命令：\r\n");
    OutW(L"    help      显示本帮助\r\n");
    OutW(L"    color     16 色 / 256 色表\r\n");
    OutW(L"    true      24 位真彩色\r\n");
    OutW(L"    cjk       中文、日文、韩文与宽字符对齐\r\n");
    OutW(L"    attr      粗体、下划线、反显等属性\r\n");
    OutW(L"    box       边框绘制\r\n");
    OutW(L"    anim      进度条动画\r\n");
    OutW(L"    big       输出 200 行，测试滚动回看\r\n");
    OutW(L"    alt       备用屏切换\r\n");
    OutW(L"    title     设置窗口标题\r\n");
    OutW(L"    exit      退出（返回码 0）\r\n");
    OutW(L"    其他      原样回显（验证键盘与粘贴）\r\n\r\n");
}

static void ShowBox() {
    OutW(L"\r\n  ┌────────────────────────────────────────────────┐\r\n");
    OutW(L"  │  主机名   web-prod-01                          │\r\n");
    OutW(L"  │  系统     Ubuntu 24.04 LTS                     │\r\n");
    OutW(L"  │  负载     0.42  0.38  0.31                     │\r\n");
    OutW(L"  │  内存     3.2G / 16G                           │\r\n");
    OutW(L"  ├────────────────────────────────────────────────┤\r\n");
    OutW(L"  │  ● nginx     \x1b[32mrunning\x1b[0m                        │\r\n");
    OutW(L"  │  ● postgres  \x1b[32mrunning\x1b[0m                        │\r\n");
    OutW(L"  │  ● redis     \x1b[31mstopped\x1b[0m                        │\r\n");
    OutW(L"  └────────────────────────────────────────────────┘\r\n\r\n");
}

// ---------------------------------------------------------------------------
static void HandleCommand(const std::string& cmdUtf8) {
    // 简单起见按 UTF-8 字节比较 ASCII 命令
    if (cmdUtf8 == "exit" || cmdUtf8 == "quit") {
        OutW(L"\r\n  再见。\r\n");
        ExitProcess(0);
    } else if (cmdUtf8 == "help") {
        ShowHelp();
    } else if (cmdUtf8 == "color") {
        ShowColors();
    } else if (cmdUtf8 == "true") {
        ShowTrueColor();
    } else if (cmdUtf8 == "cjk") {
        ShowCjk();
    } else if (cmdUtf8 == "attr") {
        ShowAttrs();
    } else if (cmdUtf8 == "box") {
        ShowBox();
    } else if (cmdUtf8 == "anim") {
        ShowProgress();
    } else if (cmdUtf8 == "big") {
        ShowScrolling();
    } else if (cmdUtf8 == "alt") {
        ShowAltScreen();
    } else if (cmdUtf8 == "title") {
        Out("\x1b]0;测试桩 - fake ssh\x07");
        OutW(L"  标题已设置为「测试桩 - fake ssh」\r\n");
    } else if (cmdUtf8 == "crash") {
        OutW(L"  模拟连接异常断开...\r\n");
        ExitProcess(255);
    } else if (cmdUtf8.empty()) {
        // 空行
    } else {
        Out("  \x1b[36mecho:\x1b[0m ");
        Out(cmdUtf8);
        Out("\r\n");
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // ConPTY 会按伪控制台的"输出代码页"解释子进程 WriteFile 的字节。
    // 中文系统默认是 936(GBK)，UTF-8 字节会被解错，所以必须先切到 65001。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_out == INVALID_HANDLE_VALUE || g_out == nullptr) return 1;

    // 把启动参数原样打出来，便于确认 SshGui 传了什么
    Out("\x1b[90m  [fake ssh] 收到的参数:");
    for (int i = 1; i < argc; ++i) {
        Out(" ");
        Out(argv[i]);
    }
    Out("\x1b[0m\r\n");

    Banner();
    Prompt();

    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string line;
    char buf[4096];

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buf, sizeof(buf), &got, nullptr) || got == 0) break;

        for (DWORD i = 0; i < got; ++i) {
            unsigned char c = (unsigned char)buf[i];

            if (c == '\r' || c == '\n') {
                if (c == '\n' && i > 0 && buf[i - 1] == '\r') continue;
                Out("\r\n");
                HandleCommand(line);
                line.clear();
                Prompt();
            } else if (c == 0x7F || c == 0x08) {
                if (!line.empty()) {
                    // 退格：按 UTF-8 字符边界删
                    size_t n = 1;
                    while (n < line.size() && ((unsigned char)line[line.size() - n] & 0xC0) == 0x80) ++n;
                    line.erase(line.size() - n);
                    Out("\b \b");
                }
            } else if (c == 0x03) {
                Out("^C\r\n");
                line.clear();
                Prompt();
            } else if (c == 0x04) {
                OutW(L"\r\n  收到 EOF，退出。\r\n");
                ExitProcess(0);
            } else if (c == 0x1B) {
                // 转义序列（方向键等）先简单跳过
            } else if (c >= 0x20) {
                line.push_back((char)c);
                char e[2] = { (char)c, 0 };
                Out(e);
            }
        }
    }

    OutW(L"\r\n  [输入流已关闭]\r\n");
    return 0;
}
