#include "session.hpp"

#include "server_client/protocol.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <istream>
#include <utility>

namespace sc {

Session::Session(uint64_t id,
                 std::function<void(uint64_t)> on_disconnect,
                 std::function<void()> on_destroyed)
    : id_(id),
      on_disconnect_(std::move(on_disconnect)),
      on_destroyed_(std::move(on_destroyed)) {}

Session::~Session() {
    // 正常情况下 write_thread_ 在 read_loop 收尾已被 join（joinable=false）。
    // 若析构时仍 joinable，说明存在未覆盖的异常路径：写线程还持有 self，
    // 而析构已在进行，此时 detach 会让写线程继续访问半析构对象（UAF 变体）。
    // 不静默掩盖：打印诊断并终止，让异常路径暴露出来。
    if (write_thread_.joinable()) {
        std::fprintf(stderr, "!!! BUG: Session 析构时写线程仍 joinable（未收尾）\n");
        std::fflush(stderr);
        std::abort();
    }
    // 通知 Server 析构完成，必须放在析构体末尾：回调在析构开始时执行的话，
    // stop() 按 expired 判断可能提前返回并析构 Server，此处再访问 Server 即
    // use-after-free（详见 server.hpp 中 destroyed_count_ 的注释）。
    if (on_destroyed_) {
        on_destroyed_();
    }
}

void Session::start() {
    // 让读写线程各自持有 shared_ptr 快照，保证线程运行期间对象存活。
    // 在同一个 lambda 里先创建写线程再执行读循环：write_thread_ 句柄必然
    // 先于 read_loop 的收尾（join 写线程）就绪，不存在句柄未赋值的中间状态
    // （若两线程在 start() 里先后赋值，读线程可能在写线程句柄赋值前就
    // 处理完命令并收尾，随后才创建的写线程会把残留响应写到已关闭的 socket）。
    // 读线程句柄就地 detach，不存入成员：读线程收尾时不再触碰自身句柄，
    // 避免与本函数对成员句柄的赋值并发访问（std::thread 对象非线程安全，
    // 并发读写是未定义行为）。线程体内持有 self，return 即释放并析构。
    auto self = shared_from_this();
    std::thread t([self] {
        try {
            self->write_thread_ = std::thread([self] { self->write_loop(); });
        } catch (...) {
            // 写线程创建失败（如系统资源不足）：本会话无法正常收发，
            // 置停止标志后走统一收尾路径，避免异常逃逸导致 std::terminate，
            // 也避免会话卡在"已注册但无线程"的中间状态。
            self->shutdown();
            self->read_loop();
            return;
        }
        self->read_loop();
    });
    t.detach();
}

void Session::shutdown() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (stopping_) {
            // 幂等：只处理一次。stopping_ 置位即保证 socket 已由本函数或
            // read_loop 收尾（统一 shutdown+close）处理，无需重复操作——
            // 置位路径只有两处：本函数（立即执行 socket 操作）与 read_loop
            // 收尾（随后必然执行 shutdown+close），不存在"置位但无人操作
            // socket"的路径。
            return;
        }
        stopping_ = true;
    }
    // 唤醒可能正在 BLOCK 等待、或等待发送队列的线程。
    stop_cv_.notify_all();
    queue_cv_.notify_all();

    // cancel 取消挂起的同步 read/write（asio 同步操作内部是异步注册 + 阻塞等待，
    // 跨平台可靠打断）；shutdown 与 send/receive 并发安全（asio 文档），让对端收到 FIN。
    // 顺序必须先 shutdown 再 cancel：若先 cancel，而读/写线程恰好处于"检查缓冲与
    // 注册 WSARecv/WSASend 之间"的窗口，cancel 没有挂起操作可取消会被丢弃，
    // 随后注册的 I/O 挂起且 shutdown 不打断已挂起的操作，会话将永久阻塞；
    // 先 shutdown 则任何"之后注册"的 I/O 都会立即失败，cancel 负责取消"已挂起"的。
    // close 由读线程收尾统一执行（与读写无并发），此处不 close。
    // 与读线程收尾的 close 互斥：close 与 cancel 并发会破坏 asio 内部状态（UB）。
    asio::error_code ignored;
    {
        std::lock_guard<std::mutex> lk(socket_mutex_);
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.cancel(ignored);
    }
}

bool Session::is_stopping() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return stopping_;
}

void Session::enqueue_response(std::string response) {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        out_queue_.push_back(std::move(response));
    }
    queue_cv_.notify_one();
}

void Session::read_loop() {
    try {
        while (true) {
            {
                std::lock_guard<std::mutex> lk(state_mutex_);
                if (stopping_) {
                    break;
                }
            }

            asio::error_code ec;
            asio::read_until(socket_, read_buffer_, '\n', ec);
            if (ec == asio::error::interrupted) {
                // 信号打断系统调用（POSIX EINTR）：read_until 未消费数据，
                // 属于可重试的瞬态错误（与 accept_loop 对 interrupted 的处理一致）。
                continue;
            }
            if (ec) {
                // 客户端断开、出错（含超长行触发 streambuf max_size 的
                // not_found），或 stop 关闭 socket 打断读操作。
                break;
            }

            // 从 streambuf 提取一行（去掉换行）。
            std::istream is(&read_buffer_);
            std::string line;
            std::getline(is, line);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();  // 兼容 CRLF。
            }

            Reply reply = process_line(line);

            // BLOCK 命令：在读线程中可中断地阻塞指定秒数。
            if (reply.block_seconds > 0) {
                std::unique_lock<std::mutex> lk(state_mutex_);
                stop_cv_.wait_for(lk, std::chrono::seconds(reply.block_seconds),
                                  [this] { return stopping_; });
                if (stopping_) {
                    break;  // stop 打断了阻塞，直接退出。
                }
            }

            // 非 QUIT 一律回复（ECHO 无参数时 text 为空串，回复空行）；
            // QUIT 直接关闭连接不回响应。
            if (!reply.close_after) {
                enqueue_response(reply.text + '\n');
            }

            if (reply.close_after) {
                break;  // QUIT：无响应直接关闭，读线程结束进入收尾。
            }
        }
    } catch (...) {
        // 读线程兜底：处理命令或 I/O 抛出的任何异常都不得使会话未清理就逃逸，
        // 这里强制进入停止状态。
        std::lock_guard<std::mutex> lk(state_mutex_);
        stopping_ = true;
    }

    // 读线程兜底清理：无论正常退出还是异常退出，都统一完成以下收尾。
    // 会话已结束，统一置 stopping_，后续 Server::stop() 的快照若仍锁定本会话，
    // shutdown() 会因幂等检查直接返回（不会对已收尾的 socket 重复操作）。
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        read_done_ = true;
        stopping_ = true;
    }
    queue_cv_.notify_all();
    stop_cv_.notify_all();

    // 等待写线程自然退出（置 write_done_ 并通知）：写线程退出即所有已入队的
    // 响应发送完毕（read_done_ 后它会把队列发空才退出），正常路径毫秒级完成，
    // PING\nQUIT\n 同包时的 PONG 不会丢失。对端不读、TCP 窗口满时写线程阻塞
    // 在 write_some（asio 同步 write 的循环实现无超时，窗口满即永久挂起），
    // 100ms 限时后强制 close 打断——Windows 的 shutdown 不打断已挂起的 WSASend，
    // 必须 close：asio 文档承诺 close 立即取消挂起的异步操作（完成
    // operation_aborted），句柄关闭后内核保证挂起 I/O 返回。
    {
        std::unique_lock<std::mutex> lk(state_mutex_);
        queue_cv_.wait_for(lk, std::chrono::milliseconds(100),
                           [this] { return write_done_; });
    }
    asio::error_code ignored;
    {
        std::lock_guard<std::mutex> lk(socket_mutex_);
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        // close 与写线程挂起的 write 并发是设计用途而非 UB，依据如下：
        // 1) asio 文档（cancel 的说明）：可移植取消应"Use the close() function
        //    to simultaneously cancel the outstanding operations and close the
        //    socket"——close 专门用于打断挂起的异步操作（完成 operation_aborted）；
        // 2) Windows：close 即 closesocket，句柄关闭后内核保证挂起的 WSASend
        //    IRP 完成并投递到 IOCP（win_iocp_socket_service_base.ipp），阻塞
        //    中的 write_some 随即返回；
        // 3) Linux：close 经 deregister_descriptor（epoll_reactor.ipp）在
        //    descriptor mutex 下把挂起操作置 operation_aborted 并通过
        //    post_deferred_completions 同步投递，阻塞中的写被唤醒返回。
        // 文档"Shared objects: Unsafe"是语言层面的保守表述，close 取消挂起
        // 操作是文档明确推荐的例外场景。
        socket_.close(ignored);
    }
    // 写线程已被唤醒（write_done_ 通知）或被 close 打断，join 必然返回。
    // 与 stop() 的 shutdown()（cancel+shutdown）互斥：cancel 与 shutdown 的并发
    // 不在 asio 的并发安全保证内，同一 socket 同时执行会破坏内部状态。
    if (write_thread_.joinable()) {
        write_thread_.join();
    }

    // 析构前通知 Server 移除本会话的 weak_ptr 记录。
    if (on_disconnect_) {
        on_disconnect_(id_);
    }

    // 本线程是会话最后一个 shared_ptr 持有者（start() 捕获的 self），
    // return 时 self 释放即触发析构。线程句柄在 start() 已 detach，此处
    // 不再触碰自身句柄成员（与 start() 的赋值并发访问是未定义行为）。
}

void Session::write_loop() {
    while (true) {
        std::string msg;
        {
            std::unique_lock<std::mutex> lk(state_mutex_);
            queue_cv_.wait(lk, [this] {
                return !out_queue_.empty() || stopping_ || read_done_;
            });
            if (out_queue_.empty()) {
                // 停止请求，或读线程已结束且队列已发空，退出。
                break;
            }
            msg = std::move(out_queue_.front());
            out_queue_.pop_front();
        }

        asio::error_code ec;
        // 信号打断（POSIX EINTR）可重试：本次 write 未写入任何数据（send 的
        // EINTR 语义），重新发送整个消息不会重复。响应都是短行（单次 send
        // 完成），不存在"部分写入后中断"的场景。
        for (;;) {
            asio::write(socket_, asio::buffer(msg), ec);
            if (ec != asio::error::interrupted) {
                break;
            }
        }
        if (ec) {
            // 写失败（对端关闭或 stop 关闭 socket）：写方向已不可用，通知读线程
            // 停止——否则读线程继续入队而队列无人消费，会无限增长。
            // 此刻未持有任何锁，shutdown() 的加锁安全（与读线程收尾的
            // socket_mutex_ 互斥等待）。
            shutdown();
            break;
        }
    }

    // 写线程退出：置退出标记并通知收尾方（等待"队列清空或写线程退出"的
    // 条件变量）。退出时队列可能未清空（写失败路径），由收尾方决定强断。
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        write_done_ = true;
    }
    queue_cv_.notify_all();
    // 不在此处关闭 socket：close 由读线程收尾统一执行（join 本线程之后）。
}

} // namespace sc
