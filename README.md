# server_client_test

基于**独立版 asio**（standalone asio，非 Boost 版本）的 TCP 服务端/客户端库，使用 C++23 编写，演示「同步 I/O + 多线程网络服务端」的正确写法。

English version: [README.en.md](README.en.md)

## 特性

| 特性 | 说明 |
|---|---|
| 多客户端 | 每连接独立 Session，互不干扰 |
| 同步 API | `read_until` / `write`，无异步回调 |
| 协程式 accept | acceptor 单线程运行 `io_context`，`accept_loop` 为 asio 协程 |
| 读写分线程 | 每连接一个读线程 + 一个写线程 |
| 优雅停止 | `stop()` 打断阻塞读写与 BLOCK 等待，等待全部会话回收后返回，幂等 |
| 可重启 | `stop()` 后可再次 `start()`（可换端口/绑定地址） |
| 可中断 BLOCK | 服务端按命令阻塞指定秒数，`stop()` 可打断 |
| 崩溃栈打印 | 进程崩溃/断言失败时自打印调用栈（`<stacktrace>`），压测不依赖调试器 |
| 双平台 | Windows（clang + MSVC STL）与 Linux（g++-14）均通过压测 |

## 线程模型

```
acceptor 线程 ── io_context.run()：协程 accept_loop（async_accept，可跨线程 cancel）
      │
      └── 每个连接：Session（自持 io_context 与 socket）
              ├── 读线程：read_until 按行解析命令（可能因 BLOCK 阻塞），响应入发送队列
              └── 写线程：从发送队列取响应，同步 write 发送
```

线程安全要点（对应 asio 文档）：

- 同一 socket 的同步 send/receive 彼此并发是线程安全的，因此读线程与写线程可安全分处两线程；
- `shutdown` 与 send/receive 并发安全，`stop()` 用它打断阻塞读写；Windows 的 shutdown 不打断已挂起的 I/O，故配合 `cancel`（IOCP 投递取消完成包）保证可靠性；
- `close` 是 asio 文档推荐的可移植取消方式（立即取消挂起的异步操作），读线程收尾用它打断阻塞写；
- Session 生命周期自管（shared_ptr 快照），`stop()` 通过 `active_sessions_` 原子计数等待全部会话析构完成，保证 Server 析构前所有 Session 已回收。

更详细的线程安全论证见 [THREAD_SAFETY.md](THREAD_SAFETY.md)。

## 协议（文本行，`\n` 分隔）

| 客户端发送 | 服务端响应 | 说明 |
|---|---|---|
| `PING` | `PONG` | 心跳 |
| `ECHO <text>` | `<text>` | 回显（无参数回空行） |
| `ADD <a> <b>` | `<a+b>` | 整数求和（含溢出检查） |
| `BLOCK <n>` | `OK`（延迟 n 秒） | 服务端在该连接的读线程阻塞 n 秒后响应，可被 stop 打断 |
| `QUIT` | 无（直接关闭连接） | 断连，客户端以读 EOF 感知 |
| 其他 | `ERROR unknown command` | 未知命令 |
| 参数非法 | `ERROR invalid ...` | 参数错误 |

命令名按任意空白字符分割（空格/tab 均可）；行长度上限 64KB，超限断开连接。

## 目录结构

```
include/server_client/
├── protocol.hpp    # 协议常量与命令解析
├── server.hpp      # Server：start/stop/port/active_connections
└── client.hpp      # Client：同步收发 + 带超时按行读取 + 主动断开
src/
├── protocol.cpp
├── server.cpp      # 协程 accept_loop + 会话回收
├── session.hpp/.cpp # 每连接会话（读线程 + 写线程 + 收尾清理）
└── client.cpp
test/
├── crash_dump.hpp      # 跨平台崩溃栈打印（Windows/Linux）
├── test_main.cpp       # 崩溃处理器安装 + gtest 入口
├── test_stress.cpp     # 压测用例（并发/竞态/连接生命周期）
├── test_slow.cpp       # 耗时用例（BLOCK/超时/connect 重试）
└── test_protocol.cpp   # 协议功能用例
stress_test.sh          # 多进程循环压测脚本（传参版）
```

## 构建与测试

依赖：vcpkg 安装的 standalone `asio` 与 `gtest`；C++23 编译器（Windows 建议 clang-cl + MSVC STL，Linux 建议 g++-13 及以上）。

### Windows

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
./build/server_client_test          # 26 个用例
```

### Linux

```bash
cmake -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-linux
./build-linux/server_client_test
```

Linux 上的崩溃栈打印需要 `-lstdc++exp`（CMakeLists 已在 UNIX 下自动链接）。

## 压测

`server_client_stress_test` 是压测专用二进制（仅快速用例，不含耗时用例），配合 `stress_test.sh` 多进程并发跑：

```bash
# 用法: stress_test.sh <可执行文件> <总次数> <进程数>
./stress_test.sh ./build/server_client_stress_test.exe 2400 12   # Windows
./stress_test.sh ./build-linux/server_client_stress_test 2400 12 # Linux
```

- 单进程运行次数 = `ceil(总次数 / 进程数)`，自动计算；
- 任一进程崩溃（`!!! CRASH`）或断言失败（`FAILED`）立即终止其余进程，并打印崩溃现场（进程自打印的调用栈，无需 gdb）；
- 压测用例通过 `--gtest_repeat` 循环运行，带 `--gtest_break_on_failure`；
- 并发度建议不超过 CPU 核数；连接风暴速率超过系统端口池（TIME_WAIT 回收）容量时客户端 connect 失败属系统限制。

已在 Windows（clang，2400 轮）与 Linux（g++-14，2400 轮）全量通过。

## 关键实现说明

- **协程式 accept**：`accept_loop` 为 `asio::awaitable` 协程，`acceptor_.cancel()` 跨线程取消挂起的 async_accept（asio 官方异步取消接口）；`io_context_.stop()` 兜底 cancel 落在注册窗口的场景；`stop()` 在关闭 acceptor 后 `restart()+poll()` 让残留协程恢复并释放其持有的会话引用，避免 Server 析构后才释放（use-after-free）。
- **会话回收**：`stop()` 通过 `active_sessions_` 原子计数（创建 +1、析构回调 -1）等待全部会话析构完成——不能用 `weak_ptr::expired()` 判断（expired 在析构体开始时即为 true，此时析构回调仍在执行，提前返回会导致回调访问已析构的 Server）。
- **写线程收尾**：读线程收尾先等写线程排空发送队列（`write_done_` 标志），限时后 `shutdown+close` 强制打断可能阻塞的写（对端不读、TCP 窗口满时同步 write 会永久阻塞）。
- **崩溃诊断**：`crash_dump.hpp` 基于 C++23 `<stacktrace>`，Windows 用 `SetUnhandledExceptionFilter`，Linux 用 `sigaction`（SIGSEGV/SIGABRT 等），崩溃/断言失败时打印自身调用栈后退出，压测无需调试器包裹。

## 质量保证

本项目在发布前经过了长时间的多轮 AI 代码审查：每一轮并发/生命周期分析都以 asio 源码与官方文档逐条证伪或证实，发现的全部问题（含竞态、use-after-free、死锁风险、协议边界）均已修复并留下依据注释。配合双平台循环压测（Windows 与 Linux 各 2400 轮全量通过）与进程自打印崩溃栈诊断，**截至目前没有发现任何已知 bug**——按本文档与 [THREAD_SAFETY.md](THREAD_SAFETY.md) 描述的方式使用，不存在已知的崩溃、竞态或资源泄漏路径。

## License

[MIT](LICENSE)
