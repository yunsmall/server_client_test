# asio 线程安全说明（basic_stream_socket 与 basic_socket_acceptor）

本文整理 asio 官方文档中 `basic_stream_socket` 与 `basic_socket_acceptor` 两个类的 **Thread Safety（线程安全）** 说明，逐句解读，并对应到本项目（`server_client_test`）的实现。

## 一、文档原文

### basic_stream_socket

> The `basic_stream_socket` class template provides asynchronous and blocking stream-oriented socket functionality.
>
> **Thread Safety**
>
> - **Distinct objects: Safe.**
> - **Shared objects: Unsafe.**
>
> Synchronous send, receive, connect, and shutdown operations are thread safe with respect to each other, if the underlying operating system calls are also thread safe. This means that it is permitted to perform concurrent calls to these synchronous operations on a single socket object. Other synchronous operations, such as open or close, are not thread safe.
>
> **Requirements**
>
> - Header: `asio/ip/tcp.hpp`
> - Convenience header: `asio.hpp`

### basic_socket_acceptor

> The `basic_socket_acceptor` class template is used for accepting new socket connections.
>
> **Thread Safety**
>
> - **Distinct objects: Safe.**
> - **Shared objects: Unsafe.**
>
> Synchronous accept operations are thread safe, if the underlying operating system calls are also thread safe. This means that it is permitted to perform concurrent calls to synchronous accept operations on a single socket object. Other synchronous operations, such as open or close, are not thread safe.

## 二、逐句解读

### 1. "Distinct objects: Safe."

不同的对象（不同的 socket / acceptor）分属不同线程各自使用，天然安全——即使它们在同一个进程里。这是所有 asio 对象都成立的基础规则。

### 2. "Shared objects: Unsafe."

**同一个对象**被多个线程同时调用成员函数，默认是**不安全**的（未定义行为）。这是总原则。下面的"并发安全"都是对总原则的**特例豁免**。

### 3. basic_stream_socket 的豁免清单（允许列表）

> Synchronous send, receive, connect, and shutdown operations are thread safe with respect to each other

在**同一个 socket 对象**上，以下**同步操作**可以跨线程并发（两两组合均可，前提是底层操作系统调用本身线程安全）：

| 操作 | 含义 | 本项目对应 |
|---|---|---|
| `send` | 发送（写） | 写线程的 `write` |
| `receive` | 接收（读） | 读线程的 `read_until` |
| `connect` | 连接 | 客户端 `connect` |
| `shutdown` | 关闭收发方向 | `stop()` 中打断阻塞读写 |

这正是「同一连接的读、写可以分处两个线程」这一设计的依据。

### 4. basic_stream_socket 的禁区

> Other synchronous operations, such as open or close, are not thread safe.

**`open` 与 `close` 不与任何并发操作共存**（包括与其他线程的 send/receive）。它们会释放/重建底层资源，与正在进行的 I/O 竞争属于未定义行为。

### 5. basic_socket_acceptor 的豁免清单

> Synchronous accept operations are thread safe

在**同一个 acceptor 对象**上，同步 `accept` 可以跨线程并发调用（比如多个线程同时 accept）。除此之外，`open`、`close` 同样是禁区。

## 三、规则速查

| 类 | 可跨线程并发 | 禁止并发 |
|---|---|---|
| `basic_stream_socket` | `send` / `receive` / `connect` / `shutdown` | `open` / `close` |
| `basic_socket_acceptor` | `accept` | `open` / `close` |

注意：`cancel` 不在文档的豁免清单中，文档对它与并发 I/O 的安全性未作明确承诺；本项目只在 `stop()` 时对每个会话的 socket 调用 `cancel` + `shutdown` 打断挂起操作，并把真正释放资源的 `close` 严格留在无并发处。

## 四、本项目中的对应实现

### 1. 读写分线程（send/receive 并发）— `src/session.cpp`

每个 Session 的**读线程只做 receive**（`read_until`）、**写线程只做 send**（`write`），互不调用对方方向的成员函数，正好落在允许列表内。

### 2. 打断阻塞读写用 shutdown，不用 close — `src/session.cpp`

`stop()` 需要打断阻塞中的 `read_until`/`write`。虽然 `close` 在实践上也能打断（Windows 尤其明显），但它不在允许列表；`shutdown` 在允许列表，且能让对端收到 FIN（优雅关闭），因此：

```cpp
// Session::shutdown()：只 cancel + shutdown，不 close
socket_.cancel(ignored);
socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
```

### 3. close 只发生在无并发处 — `src/session.cpp` / `src/server.cpp`

- **Session 的 socket**：读线程收尾时**先 join 写线程**、自身也已停止读写，然后才 `close`——此时没有任何线程再用该 socket；
- **acceptor**：`open/bind/listen` 在 acceptor 线程**启动之前**；`close` 在 join acceptor 线程**之后**。

### 4. acceptor 只在 acceptor 线程操作 — `src/server.cpp`

`stop()` **不调用 acceptor 的任何成员函数**，而是「连接一次自身端口」唤醒阻塞中的 accept——连接操作作用在新建的 socket 上，与 acceptor 无关，不构成并发访问：

```cpp
// Server::stop()：唤醒阻塞的 accept，acceptor 本身不被跨线程触碰
asio::ip::tcp::socket wake_socket(io_context_);
wake_socket.connect(actual_endpoint_, wake_ec);
// accept 返回唤醒连接后检查 running_，退出 accept_loop
```

### 5. 客户端对象单线程使用 — `include/server_client/client.hpp`

`Client` 的 `disconnect`（shutdown + close）与 `read_line` 不保证并发安全，头文件注释明确：

```cpp
// 注意：非线程安全——单个 Client 对象应由单个线程使用。
```

## 五、常见误区

1. **「asio 的 socket 是线程安全的」**——不准确。只有 `send/receive/connect/shutdown` 彼此并发安全；`open/close` 是禁区。
2. **「close 可以打断阻塞的同步读」**——Windows 上实践可行（`closesocket` 打断阻塞的 recv），**Linux 上不可行**（`close()` 不唤醒另一线程阻塞中的 `accept()`/`recv()`）；且文档层面 `close` 与并发 I/O 不安全。正确姿势：用 `shutdown`（必要时加 `cancel`）打断，`close` 放在无并发处。
3. **「acceptor 和 socket 的规则一样」**——不同：acceptor 豁免的是 `accept`（可并发），socket 豁免的是 `send/receive/connect/shutdown`；两者共同禁区是 `open/close`。
4. **「同步操作就是裸系统调用」**——asio 的同步操作内部是「异步注册 + 阻塞等待」的实现，因此 `cancel` 可以跨平台打断阻塞中的同步读写（Linux 上 `shutdown` 还能让阻塞的 recv 直接返回 EOF）。
