#pragma once

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace sc {

class Session;

// TCP 服务端。
// 生命周期契约：析构不自动 stop()。析构前必须显式调用 stop()，
// 否则运行中的 acceptor 线程会因 std::thread 仍 joinable 而直接 std::terminate，
// 且存活会话析构时会通过回调访问已析构的 Server（未定义行为）。
// 线程模型：acceptor 在单独一个线程运行；每接受一个客户端连接，
// 为该连接再启动一个读线程和一个写线程（读写均使用同步 API）。
// 依据 asio 文档，同一 socket 的读写操作只要操作系统线程安全即可分处不同线程，
// 因此每个连接由一个读线程读、一个写线程写。
class Server {
public:
    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 启动服务端（幂等）：监听参数在此传入，port 传 0 表示由系统分配可用端口
    // （可经 port() 查询实际端口），bind_ip 为监听地址。stop() 之后可再次调用
    // 以重启（可传入不同参数）。
    void start(uint16_t port = 0, std::string bind_ip = "127.0.0.1");

    // 停止服务端（幂等）：关闭 acceptor、优雅中止所有会话并等待全部自析构。
    // 会打断正在阻塞的读写与 BLOCK 等待，返回时所有连接已关闭、会话已回收。
    // 析构前必须调用本函数（析构不自动 stop，见类注释契约）。
    void stop();

    // 当前监听端口（start 后为实际监听端口，未 start 或已 stop 时为 0）。
    uint16_t port() const;

    // 当前活跃连接数（供测试断言）。
    std::size_t active_connections() const;

private:
    // 协程式接受循环：在 acceptor 线程的 io_context 上调度（co_spawn），
    // 每轮创建一个会话并异步接受连接，stop() 用 acceptor_.cancel() 取消。
    asio::awaitable<void> accept_loop();
    // 会话析构前调用：加锁从连接记录中移除自身的 weak_ptr。
    void remove_session(std::uint64_t id);

    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    // 实际监听端口：start() 在 bind 后（无并发）查询一次存入，port() 直接读
    // 原子值——避免运行期查询 acceptor（local_endpoint 与 acceptor 线程的
    // async_accept 并发不在 asio 的共享对象线程安全保证内）。
    std::atomic<uint16_t> port_{0};

    // 保护 acceptor 的取消操作（stop() 在锁内 cancel 挂起的异步 accept）。
    // 协程的注册发生在 co_await 求值（await_suspend）时，无法持锁，
    // 取消的可靠性由 stop() 的 io_context_.stop() 兜底（run() 立即返回）。
    // running_ 为原子变量，start/stop/accept_loop 各自独立读写，无需锁。
    mutable std::mutex acceptor_mutex_;
    std::atomic<bool> running_{false};
    std::thread acceptor_thread_;

    // 保护活跃连接表
    mutable std::mutex conn_mutex_;
    // 存活会话计数（原子，无需锁）：accept_loop 创建会话后立即 +1，
    // 会话析构体末尾的 on_destroyed 回调 -1。stop() 等待它归零，语义是
    // "所有曾创建的会话都已析构完成"——与 connections_ 快照解耦：
    // 会话在 remove_session 与析构完成之间的窗口（已从表移除但析构未完）
    // 不会被快照漏掉，也不会被其他会话的析构提前满足等待条件。
    // 不能用 weak_ptr::expired() 判断——expired 在析构体开始时就为 true，
    // 此时析构体仍在执行，stop() 提前返回并析构 Server 会撞上仍在执行的
    // 析构回调（其访问 Server 成员，use-after-free）。
    std::condition_variable reap_cv_;
    std::atomic<std::size_t> active_sessions_{0};
    // Server 仅持 weak_ptr 观察：Session 生命周期由自身管理
    // （读线程作为主线程，return 时最后一个引用释放即自析构）。
    // weak_ptr 供 stop() 锁定存活会话以 shutdown，以及查询活跃数。
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> connections_;
    std::atomic<std::uint64_t> next_id_{1};
};

} // namespace sc
