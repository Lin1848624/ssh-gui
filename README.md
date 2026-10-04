# SSH GUI

![图标](docs/icon.png)

Windows 上的图形化 SSH 终端。纯 C++ / Win32 + GDI+ 写的原生程序，不依赖任何第三方
GUI 框架或网络库；SSH 协议本身交给系统自带的 OpenSSH 客户端，界面通过 ConPTY
把它当成一个真终端来驱动。

**单个 exe，约 540 KB，拷贝即用**（静态链接 CRT，不需要装运行库）。

---

## 功能

| 分类 | 说明 |
| --- | --- |
| 会话管理 | 主机 / 端口 / 用户名 / 私钥 / 启动目录 / 压缩 / 保活 / 详细日志；侧边栏列表，双击直接连接 |
| 密码保存 | 用 Windows DPAPI 加密后存盘，绑定当前用户账户，换用户或换机器都解不开 |
| 多标签 | 一个窗口里开多个会话，标签上带连接状态点（绿=已连接 / 黄=连接中 / 红=已断开） |
| 终端模拟 | 完整 VT/ANSI 解析：CSI/SGR、256 色与 24 位真彩色、备用屏、括号粘贴、鼠标上报、DEC 制图字符集 |
| 中文显示 | 终端子树全部按 UTF-8 处理，中文、日文、韩文、Emoji 都能正确显示与对齐 |
| 滚动回看 | 8000 行历史缓冲，滚轮 / Ctrl+Home / Ctrl+End / Ctrl+PageUp / Ctrl+PageDown |
| 字体缩放 | Ctrl+Shift+加减号，Ctrl+Shift+0 复位 |
| 端口转发 | 界面里配置 `-L` 本地 / `-R` 远程 / `-D` 动态(SOCKS5) 三类隧道，支持多条 |
| 文件传输 | SFTP 面板：左右双栏浏览本地与远端，上传、下载、删除、新建目录、双击进目录 |
| 剪贴板 | 选中即复制、右键粘贴、Ctrl+Shift+C/V、Ctrl+Shift+A 全选，支持 OSC 52 |
| 其它 | 标签标题跟随远端 `OSC 0/1/2`、终端响铃闪烁窗口、会话结束显示退出码 |

### 键盘

| 按键 | 作用 |
| --- | --- |
| `Ctrl+Shift+C` / `Ctrl+C` | 复制（有选中内容时；无选中时 `Ctrl+C` 照常发中断） |
| `Ctrl+Shift+V` / 右键 / 中键 | 粘贴 |
| `Ctrl+Shift+A` | 全选 |
| `Ctrl+Shift+加/减/0` | 放大 / 缩小 / 复位字号 |
| `Ctrl+Home` / `Ctrl+End` | 跳到回看缓冲区顶部 / 底部 |
| `Ctrl+PageUp` / `Ctrl+PageDown` | 上翻 / 下翻一屏 |
| 滚轮 | 上下回看 |
| 双击侧边栏会话 | 直接连接 |

---

## 快速开始

1. 确保系统里有 **OpenSSH 客户端**（Windows 10 1809+ 一般自带）：

   ```
   设置 -> 系统 -> 可选功能 -> 添加功能 -> OpenSSH 客户端
   ```

   本程序会在 `PATH` 里找 `ssh.exe` / `sftp.exe`，找不到时会给出明确提示。

2. 运行 `SshGui.exe`，点左上角 **新建连接**，填主机地址和用户名。
3. 左侧选中会话，点 **连接**（或直接双击会话项）。
4. 首次连接某台主机会像命令行 ssh 一样询问是否信任主机指纹，在终端里输入 `yes` 回车即可。
5. 需要传文件时点 **文件传输** 打开 SFTP 面板。

### 系统要求

- Windows 10 1809（build 17763）或更高 —— 需要 ConPTY。
- 一个可用的 `ssh.exe`（OpenSSH 客户端）。

---

## 从源码编译

只需要 Visual Studio 生成工具（MSVC v143）+ Windows SDK。

```bat
build.bat            :: Release
build.bat debug      :: Debug
build.bat release w4 :: Release 并用 /W4 全量检查
build.bat clean      :: 清理 obj/ 与 bin/
```

脚本会自己定位 `vcvars64.bat`（先试 `D:\App\VS-BuildTools`，再用 `vswhere.exe` 兜底），
不需要事先打开 Developer Command Prompt。产物是 `bin\SshGui.exe`。

图标重新生成（需要 Pillow）：

```powershell
python tools\make_icon.py
```

`tests\` 下是端到端测试用的桩程序，不参与主程序构建，用法见「测试」一节。

---

## 项目结构

```
src/
  common.h/.cpp      公共环境、DPI 缩放、字符串转换、日志、剪贴板
  theme.h            配色与字体常量（终端 ANSI 16 色用 Campbell 方案）
  json.h/.cpp        极简 JSON 读写（会话配置持久化）
  session.h/.cpp     会话模型、DPAPI 密码保护、ssh/sftp 命令行构造
  conpty.h/.cpp      ConPTY 伪终端进程封装（Job Object + 非阻塞读取）
  vt.h/.cpp          VT/ANSI 解析器与字符网格（不涉及任何窗口/绘图）
  terminal.h/.cpp    终端视图子窗口：网格渲染、键盘鼠标、选区、滚动
  widgets.h/.cpp     GDI+ 绘图辅助、自绘按钮、深色标题栏
  dialogs.h/.cpp     会话编辑对话框、端口转发编辑、简易输入框
  sftppanel.h/.cpp   SFTP 文件传输窗口（sftp.exe 批处理模式 + ls -l 解析）
  main.cpp           主窗口：工具条、会话侧边栏、多标签、状态栏、程序入口
res/                 图标与版本信息
tests/               端到端测试桩（fake_ssh / fake_sftp / gdi_probe）
docs/                README 用图
```

---

## 实现要点

这一节记录几个踩过的坑，都是「看起来能跑但结果是错的」那类。

### 1. 中文乱码：ConPTY 的伪控制台有自己的代码页

ConPTY 会把子进程 `WriteFile` 出去的**字节**按伪控制台的**输出代码页**解释成字符。
中文系统上这个代码页默认是 `936 (GBK)`，而远端 Linux 发来的是 UTF-8 ——
结果整片乱码，典型症状是「锟斤拷」。

代码页是**控制台对象**的属性，不是进程的，所以没法在我们的进程里设。做法是用
`cmd.exe` 包一层，在**同一个伪控制台**里先 `chcp 65001` 再拉起 ssh：

```
cmd.exe /c chcp 65001 >nul & "C:\path\to\ssh.exe" -tt -p 22 user@host
```

ssh 继承同一个控制台对象，于是输入输出都按 UTF-8 走。

### 2. 这层启动器不能是本程序自己

一开始想让 `SshGui.exe --pty-launch` 兼任启动器，结果终端一片空白，还多出一个黑窗口。
原因：**SshGui 是 GUI 子系统程序**，作为 ConPTY 的直接子进程时并不会附加到伪控制台，
于是它再启动的 ssh 被系统另外分配了一个真实控制台窗口，输出全跑到那个窗口里去了。

`cmd.exe` 是控制台程序，能正确附加，所以用它当这一层。同理，`--askpass` 助手模式
是纯 stdout 输出、不碰控制台，用自己没问题。

### 3. 制表符必须自己画

`Consolas` 里没有 U+2500 区的字形，GDI 会靠字体链接回退到 `SimSun`；而 SimSun 的框线
是 1 物理像素的细线，在 14px 字号下被抗锯齿磨得几乎看不见 —— 终端里只剩几个角上的竖线，
横线整片消失，所有 TUI 边框全废。

解决办法是 `DrawBoxChar()` 自己用 `FillRect` 画单线/双线/单双混合的框线字符。
顺带解决了另一个问题：相邻格子的线能严丝合缝地接上。目前覆盖 `U+2500`–`U+257F`。

### 4. 读线程不能死等 ReadFile

子进程退出后 ConPTY 并不会立刻关掉输出管道，阻塞式 `ReadFile` 会一直挂着，
界面上永远看不到「会话已结束」。改成先 `PeekNamedPipe` 探可读字节数，没数据时
顺便看一眼进程是否已退出，退出且静默约 300ms 才收尾 —— 既不丢结尾的输出，也能及时收尾。

### 5. Job Object 保证进程树能被收干净

因为有 `cmd.exe` 这一层，`TerminateProcess` 只会杀掉 cmd，ssh 会变成孤儿进程继续挂着。
所以启动时用 `CREATE_SUSPENDED` 建进程，塞进带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`
的作业对象后再 `ResumeThread`，退出时 `TerminateJobObject` 一次收掉整棵树。

### 6. 对话框不能 PostQuitMessage

模态对话框的循环是 `while (IsWindow(h) && GetMessage(...))`。如果窗口先销毁、条件短路，
队列里那条 `WM_QUIT` 就没被取走，回到主消息循环时会把**整个程序**关掉。
对话框退出只靠 `IsWindow()` 判断即可，不要投 `WM_QUIT`。

### 7. `WM_CTLCOLOR*` 不能返回 `NULL_BRUSH`

`NULL_BRUSH` 的含义是「不填充」。EDIT / LISTBOX 拿到它就不会擦背景，
旧文字会一层层叠上去，看起来像文字重影。必须返回一个真实的深色画刷。

### 8. ListBox 自绘项的 `rcItem` 带偏移

`LBS_OWNERDRAWFIXED` 的 `WM_DRAWITEM` 里，第 N 项的 `rcItem.top` 是 `N * itemHeight`。
按 `(0,0,w,h)` 去画的话，所有项都会叠在第一行上，表现为「列表里只有一项」。

### 9. 滚轮方向

`WM_MOUSEWHEEL` 的 delta 为正表示滚轮向前（远离用户），对应**往上看历史**，
视口偏移要**增大**。写反的表现是滚轮完全没反应（偏移被钳在 0）。

---

## 配置文件

| 文件 | 位置 | 说明 |
| --- | --- | --- |
| 会话配置 | `%LOCALAPPDATA%\SshGui\sessions.json` | UTF-8 JSON，可直接手工编辑 |
| 运行日志 | `%LOCALAPPDATA%\SshGui\sshgui.log` | 超过 2 MB 自动轮转成 `.log.1` |

密码字段是用 DPAPI 加密后的 `dpapi:<base64>`，只能在保存它的那个 Windows 用户下解开。

---

## 测试

`tests/` 里的桩程序用来在没有真实服务器的情况下做端到端验证：

- `fake_ssh.cpp` —— 编译成 `ssh.exe`，输出各种 ANSI 序列（颜色表、中文对齐、属性、
  进度条动画、备用屏切换）并回显输入，用来验证伪终端集成与 VT 渲染。
- `fake_sftp.cpp` —— 编译成 `sftp.exe`，按 OpenSSH 的真实输出格式回话，验证 SFTP 面板
  与 `ls -l` 解析。
- `gdi_probe.cpp` —— 隔离验证 GDI 字形回退行为的小工具。

用法：把桩程序编译到某个目录，把该目录放在 `PATH` 最前面再启动本程序，
新建一个指向任意主机的会话即可。真实 `ssh.exe` 的验证同样做过（本机 sshd，
主机指纹确认 + 密码提示均正常）。

编译产物已经 200 行以上的 `/W4` 零警告检查。

---

## FAQ

**连不上，提示找不到 ssh.exe？**
装 OpenSSH 客户端（见「快速开始」），或把 OpenSSH 目录加进 `PATH` 后重启本程序。

**终端里中文是乱码 / 方框？**
正常情况下不会。若出现，请把 `%LOCALAPPDATA%\SshGui\sshgui.log` 一起提供 —— 多半是
系统里 `cmd.exe` 或代码页设置异常。

**首次连接弹指纹确认，能自动接受吗？**
在会话的「额外参数」里填 `-o StrictHostKeyChecking=accept-new`。
默认保持与命令行 ssh 一致的手动确认，避免中间人风险。

**能保存密码吗？**
可以。勾选「保存密码」后会用 DPAPI 加密存盘，并在连接时通过 `SSH_ASKPASS` 自动填入。
不勾选则每次在终端里手输。

**支持密钥认证吗？**
支持。「私钥文件」填私钥路径即可（会带 `-i` 和 `IdentitiesOnly=yes`）。
也可以用 ssh-agent，什么都不填时 ssh 会自己按默认顺序尝试。

**SFTP 支持传整个目录吗？**
暂不支持递归上传/下载，需要进目录后逐个文件传。

---

## 许可证

Copyright (C) 2026 Lin1848624

本项目以 **GNU General Public License v3.0** 发布，完整条款见 [LICENSE](LICENSE)。

你可以自由使用、修改和分发本程序；但如果分发修改后的版本，必须同样以
GPL-3.0（或更高版本）开放源代码，并附上完整许可证文本。本程序不提供任何担保。
