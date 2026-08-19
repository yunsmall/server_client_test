#pragma once

#include <asio.hpp>
#include <asio/streambuf.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace sc {

// 单个客户端连接的会话，内部持有读写两个线程。
// 读线程：同步 read_until 按行读取并解析命令（可能因 BLOCK 而阻塞），
//         并作为会话主线程兜底处理异常、停止与关闭。
// 写线程：从发送队列取出响应，同步 write 发送。
// 同一 socket 只被读线程读、只被写线程写，满足 asio 的线程安全前提。
// 生命周期自管：读写线程持有 shared_ptr 快照，读线程 return 时最后一个引用释放即析构
// （析构前通知 Server 移除其 weak_ptr 记录；读线程句柄在 start() 就地 detach，不存成员，
// 避免读线程收尾与 start() 赋值并发访问 std::thread 对象）。
class Session : public std::enable_shared_from_this<Session> {
public:
    // on_disconnect 在会话结束（读线程退出前）时调用，用于让 Server 移除自身 weak_ptr。
    // on_destroyed 在会话析构体末尾调用，用于通知 Server 析构完成（stop() 等待回收）。
    // 注意：回调必须放在析构体末尾——若放在开头，stop() 按 expired 判断可能在其
    // 执行期间返回并析构 Server，回调访问 Server 成员即为 use-after-free。
    // socket 由本会话自持（见成员 socket_），accept_loop 接受连接时直接接受进它，
    // 连接的生命周期完全由本会话自己管理，不依赖 Server 的 io_context。
    Session(uint64_t id,
            std::function<void(uint64_t)> on_disconnect,
            std::function<void()> on_destroyed);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // 启动读写线程。
    void start();

    // 请求优雅停止：置停止标志、唤醒阻塞的读写与 BLOCK 等待、shutdown+close 打断同步 I/O。
    void shutdown();

    uint64_t id() const { return id_; }

    // 本会话自持的 socket（关联本会话的 io_context_），仅供 accept_loop 接受连接用。
    asio::ip::tcp::socket& socket() { return socket_; }

private:
    void read_loop();
    void write_loop();
    void enqueue_response(std::string response);
    bool is_stopping() const;

    uint64_t id_;
    // 声明在 socket_ 之前：析构时 socket_ 先析构（上下文还活着），
    // 随后 io_context_ 才销毁，顺序安全。
    asio::io_context io_context_;            // 本会话自持的处理上下文
    asio::ip::tcp::socket socket_{io_context_};  // 关联自持上下文，由 accept_loop 接受连接
    std::function<void(uint64_t)> on_disconnect_;
    std::function<void()> on_destroyed_;

    // 以下状态由 state_mutex_ 保护，queue_cv_ / stop_cv_ 均关联该锁。
    mutable std::mutex state_mutex_;
    std::condition_variable queue_cv_;  // 等待发送队列非空或会话结束
    std::condition_variable stop_cv_;   // BLOCK 阻塞等待被打断
    std::deque<std::string> out_queue_; // 待发送的响应（含换行）
    bool stopping_ = false;             // stop 请求已发出
    bool read_done_ = false;            // 读线程已结束
    bool write_done_ = false;           // 写线程已退出（收尾等待队列清空时使用）

    // 保护 socket 的关闭类操作：读线程收尾的 close 与 stop() 的 cancel/shutdown
    // 分属不同线程，close 与 cancel 并发会破坏 asio 内部状态（未定义行为），必须互斥。
    // 读写线程的 send/receive 不经此锁（asio 文档允许与 shutdown 并发）。
    std::mutex socket_mutex_;

    // 单行长度上限：read_until 读到该上限仍未出现换行会以 error::not_found
    // 返回（streambuf 的 max_size 机制，见 asio/impl/read_until.hpp），读循环
    // 视作连接断开。防止恶意客户端发送无换行的超长数据无限膨胀内存。
    static constexpr std::size_t kMaxLine = 64 * 1024;
    asio::streambuf read_buffer_{kMaxLine};  // 读线程按行解析用的缓冲
    std::thread write_thread_;               // 写线程句柄（仅读线程访问：赋值与 join）
};
} // namespace sc
