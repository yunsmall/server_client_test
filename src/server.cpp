#include "server_client/server.hpp"

#include "session.hpp"

#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sc {

// 诊断：进程启动起算的毫秒时间戳，与 session 侧打印对应。
static const auto kProcStart = std::chrono::steady_clock::now();


Server::Server()
    : io_context_(),
      acceptor_(io_context_) {}

Server::~Server() {
    // 析构不自动 stop：Server 生命周期由调用方管理（契约见 server.hpp 类注释）。
    // 析构前必须已显式调用 stop()，否则 acceptor 线程仍 joinable 会 std::terminate，
    // 且存活会话析构时会通过回调访问已析构的 Server（未定义行为）。
}

void Server::start(uint16_t port, std::string bind_ip) {
    // 幂等检查放最前：已在运行时不解析地址、不修改任何状态（幂等语义优先）。
    if (running_.load()) {
        return;  // 已在运行。
    }

#ifdef SIGPIPE
    // 对端关闭连接后继续发送会触发 SIGPIPE（默认终止进程），服务器应忽略：
    // 写失败统一走 error_code 路径处理。asio 的 socket 发送已带 MSG_NOSIGNAL，
    // 此处显式忽略作为跨版本防御（重复调用无害）。
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // 监听端点由本次启动参数决定（非法地址在此抛异常，此时尚未修改运行状态）。
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(bind_ip), port);

    running_.store(true);

    // 重新初始化 acceptor（可能因之前的 stop 被 close，不可复用）。
    asio::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (!ec) {
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    }
    if (!ec) {
        acceptor_.bind(endpoint, ec);
    }
    if (!ec) {
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    }
    if (ec) {
        running_.store(false);
        // 清理半初始化状态：open 后 bind/listen 失败时 acceptor 仍处于打开状态，
        // 不关闭会导致下次 start() 的 open 失败（不能重复 open），服务无法恢复。
        asio::error_code ignored;
        acceptor_.close(ignored);
        throw std::runtime_error("server start failed: " + ec.message());
    }

    // 记录实际监听端口（port 传 0 时由系统分配，需查询）。此处 acceptor 线程
    // 尚未启动，local_endpoint 查询无并发；之后 port() 只读原子值，不再触碰
    // acceptor（local_endpoint 与 acceptor 线程的 async_accept 并发不在 asio
    // 共享对象线程安全保证内）。
    {
        asio::error_code ep_ec;
        auto ep = acceptor_.local_endpoint(ep_ec);
        port_.store(ep_ec ? 0 : ep.port());
    }

    // 复用 io_context：上一次 run() 返回后它处于停止状态，必须 restart
    // 才能再次运行（否则本次 run() 立即返回，协程不会执行）。
    io_context_.restart();

    // acceptor 线程：调度接受协程并运行 io_context（连接事件的完成处理器
    // 与协程恢复都在本线程执行）。协程被 stop() 的 cancel 取消后无待办
    // 工作，run() 返回，线程退出。
    try {
        acceptor_thread_ = std::thread([this] {
            asio::co_spawn(io_context_, accept_loop(), asio::detached);
            io_context_.run();
        });
    } catch (...) {
        // 线程创建失败（如系统资源不足）：恢复未运行状态并关闭 acceptor，
        // 避免服务卡在"已标记运行但无 acceptor 线程"的中间状态。
        running_.store(false);
        asio::error_code ignored;
        acceptor_.close(ignored);
        throw;
    }
}

void Server::stop() {
    // 原子置停止标志：与 start()/accept_loop 的原子读写互不阻塞。
    if (!running_.exchange(false)) {
        // 防御：即便从未运行，也确保不残留可 join 的线程。
        if (acceptor_thread_.joinable()) {
            acceptor_thread_.join();
        }
        return;
    }

    // 在锁下取消挂起的异步 accept（async_accept 是 asio 支持跨线程取消的
    // 异步操作，cancel() 是其官方取消接口）：完成处理器收到 operation_aborted
    // 后协程检查 running_ 退出。协程可能在"accept 完成与重新注册之间"的
    // 窗口错过本次取消，此时由下方 io_context_.stop() 兜底。
    {
        std::lock_guard<std::mutex> lk(acceptor_mutex_);
        asio::error_code ignored;
        acceptor_.cancel(ignored);
    }

    // 兜底：即使 cancel 落在协程的注册窗口（没有挂起的 accept 可取消），
    // 也能让 acceptor 线程的 run() 立即返回，join 不会永久等待。
    // 残留的协程在下一次 start()（restart 后 run）或 io_context 析构时清理。
    io_context_.stop();

    if (acceptor_thread_.joinable()) {
        acceptor_thread_.join();
    }

    // acceptor 线程已退出，此处关闭 acceptor 无并发，安全。
    asio::error_code ignored;
    acceptor_.close(ignored);
    port_.store(0);  // 端口已失效，port() 返回 0（未监听语义）。

    // 让残留的接受协程在 stop() 内结束：close 会取消挂起的异步 accept 并投递
    // 完成事件，而前面的 io_context_.stop() 已让 run() 返回、事件滞留队列。
    // 若放任不管，协程帧会一直持有其创建的 Session（shared_ptr）直到下一次
    // start() 或 Server 析构时 io_context 销毁才释放——届时会话析构回调访问的
    // Server 可能已析构（use-after-free）。restart 后 poll 处理这些完成事件，
    // 协程恢复后检查 running_ 为 false 即 co_return，帧销毁、会话引用释放。
    // poll() 一次即足够（源码依据）：asio 各后端取消/关闭挂起操作时都是
    // 同步投递完成事件——win_iocp 的 cancel/close 把完成包投递进 IOCP 队列
    // （即投递即就绪）；Linux epoll 的 cancel_ops / deregister_descriptor
    // （epoll_reactor.ipp）通过 scheduler_.post_deferred_completions 把
    // operation_aborted 的完成事件同步放入调度队列，不依赖 epoll_wait 唤醒。
    // 因此 close 之后 poll() 必然处理到全部取消完成事件。恢复的协程收到
    // operation_aborted 必然 co_return，不会与下一次 start() 的新协程并存。
    io_context_.restart();
    io_context_.poll();

    // 快照当前存活会话：weak_ptr 锁定成功说明会话仍存活，需要统一停止。
    std::vector<std::shared_ptr<Session>> conns;
    {
        std::lock_guard<std::mutex> lk(conn_mutex_);
        conns.reserve(connections_.size());
        for (auto& [id, w] : connections_) {
            if (auto s = w.lock()) {
                conns.push_back(std::move(s));
            }
        }
    }

    // 请求所有存活会话优雅停止（打断阻塞读写与 BLOCK 等待）。
    for (auto& c : conns) {
        c->shutdown();
    }

    // 释放引用：读线程被打断后退出，最后一个引用释放即自析构。
    conns.clear();

    // 等待存活会话计数归零：accept 已停止，不再有新会话创建，计数只会单调
    // 递减到 0，语义是"所有已创建会话的析构体均已执行完"（析构体末尾回调
    // 递减，详见 server.hpp 中 active_sessions_ 的注释）。等待与 connections_
    // 快照解耦：会话在"remove_session 后、析构完成前"的窗口不会被漏掉。
    {
        std::unique_lock<std::mutex> lk(conn_mutex_);
        if (!reap_cv_.wait_for(lk, std::chrono::seconds(10), [&] {
                return active_sessions_.load() == 0;
            })) {
            // 诊断：10 秒内未归零说明有会话引用泄漏或线程阻塞未退出，打印
            // 现场后继续等待——高负载下会话回收可能超过 10 秒，硬终止会误杀
            // 正常慢速路径，这里只留痕不中断（配合崩溃处理器与日志定位）。
            std::fprintf(stderr,
                         "!!! LEAK [+%lldms] stop() 等待会话析构超时（剩余 %zu 个活跃会话）\n",
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - kProcStart)
                             .count(),
                         active_sessions_.load());
            std::fflush(stderr);
            reap_cv_.wait(lk, [&] { return active_sessions_.load() == 0; });
        }
    }

    // 清空连接记录（已自析构的会话其记录已在析构前被 remove_session 移除）。
    {
        std::lock_guard<std::mutex> lk(conn_mutex_);
        connections_.clear();
    }
}

uint16_t Server::port() const {
    // 直接返回 start() 时记录的原子值：运行期不触碰 acceptor，避免与
    // acceptor 线程的 async_accept 并发（共享对象并发不在 asio 保证内）。
    return port_.load();
}

std::size_t Server::active_connections() const {
    std::lock_guard<std::mutex> lk(conn_mutex_);
    // 只统计仍存活的会话：已断开但尚未完成析构的会话（weak_ptr 已过期）不计，
    // 语义是"当前可用的连接数"，避免记录残留导致计数偏高。
    std::size_t n = 0;
    for (const auto& [id, w] : connections_) {
        if (!w.expired()) {
            ++n;
        }
    }
    return n;
}

asio::awaitable<void> Server::accept_loop() {
    while (true) {
        if (!running_.load()) {
            co_return;
        }

        uint64_t id = next_id_.fetch_add(1);
        // 先创建会话（内部自持 io_context 与 socket），连接异步接受进会话的 socket。
        auto conn = std::make_shared<Session>(
            id,
            [this](uint64_t cid) { remove_session(cid); },
            [this] {
                // 会话析构体末尾调用：递减存活计数并唤醒 stop() 的等待
                // （计数语义见 server.hpp 中 active_sessions_ 的注释）。
                // 递减必须在 conn_mutex_ 下进行：stop() 的谓词检查与进入等待
                // 以同一把锁同步，若此处不持锁，递减+通知可能落在"谓词检查
                // 与 wait 之间"的窗口而被丢弃（lost wakeup），stop() 误超时。
                {
                    std::lock_guard<std::mutex> lk(conn_mutex_);
                    active_sessions_.fetch_sub(1);
                }
                reap_cv_.notify_all();
            });
        // 创建即计入存活：会话可能从未接受连接（协程被取消），但其析构回调
        // 同样递减，stop() 的"等待归零"语义覆盖所有已创建会话。
        active_sessions_.fetch_add(1);

        // 注册前检查：缩小 stop() 取消丢失的窗口（最终由 io_context_.stop() 兜底）。
        // 注册发生在 co_await 求值（await_suspend）时，无法持锁进行"检查+注册"。
        if (!running_.load()) {
            co_return;
        }

        // 异步 accept：stop() 通过 acceptor_.cancel() 跨线程取消挂起的 accept
        // （cancel 是 asio 官方的异步取消接口）。协程挂起期间 conn 由协程帧持有。
        asio::error_code ec;
        co_await acceptor_.async_accept(conn->socket(),
                                        asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            // 瞬态错误（对端在 accept 前重置/中止连接）在连接频繁建立断开的
            // 高并发下很常见，直接退出会让服务端静默停止 accept，必须继续循环。
            // 致命错误（acceptor 被取消/关闭）由 stop() 的 running_ 标志接管退出。
            if (ec == asio::error::connection_reset ||
                ec == asio::error::connection_aborted ||
                ec == asio::error::interrupted) {
                continue;
            }
            co_return;
        }

        // 可能是 stop() 取消后仍完成的连接（竞态），丢弃并退出。
        if (!running_.load()) {
            co_return;
        }

        try {
            std::lock_guard<std::mutex> lk(conn_mutex_);
            // 仅存 weak_ptr：Session 生命周期自管，Server 不持有引用。
            connections_.emplace(id, conn);
        } catch (...) {
            // 连接表插入失败（如内存不足）：放弃该会话——active_sessions_ 已
            // 在创建时递增，conn 离开作用域即析构（回调递减），计数保持平衡。
            // 不能抛给 detached 协程（会终止接受循环），继续 accept。
            continue;
        }
        try {
            conn->start();
        } catch (...) {
            // 读线程创建失败（如系统资源不足）：会话没有线程、不会自行收尾，
            // 移除其连接记录并放弃该会话（conn 离开作用域即析构，析构回调
            // 递减 active_sessions_，计数保持平衡）。不能重抛：detached 协程
            // 的未捕获异常会让 acceptor 协程终止，服务器静默停止接受连接；
            // 此处继续 accept 循环，资源恢复后即可正常服务。
            remove_session(id);
            continue;
        }
    }
}

void Server::remove_session(uint64_t id) {
    // 会话析构前调用（读线程收尾），移除自身的 weak_ptr 记录。
    std::lock_guard<std::mutex> lk(conn_mutex_);
    connections_.erase(id);
}

} // namespace sc
