#include "server_client/client.hpp"

#include <cerrno>

#ifndef _WIN32
#include <poll.h>
#endif

namespace sc {

namespace {

// 单次 poll 的最长等待：poll 的 timeout 参数是 int（约 24.8 天封顶，更大值
// 转成负数会变成无限等待），且过长的单次等待不利于及时响应 deadline。
// 超时更长时按段等待，每段结束回到循环重算剩余时间。
constexpr long kMaxPollMs = 60 * 1000;  // 1 分钟。

// 阻塞等待 socket 可读，最晚到 deadline（不突破调用方给定的超时）。
// 返回：1 = 可读，0 = 超时，-1 = 出错。
// 用 poll/WSAPoll 而非 select：select 的 fd_set 有 FD_SETSIZE(1024) 上限，
// 高负载下 fd 超过 1024 时 FD_SET 越界（glibc 直接 abort），poll 无此限制。
// 信号打断（EINTR/WSAEINTR）属瞬态错误：回到循环重新 poll，每次重试都按
// deadline 重算剩余时间，反复打断也不会拖出调用方给定的超时上限。
int wait_readable(asio::ip::tcp::socket& socket,
                  std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        long remaining_ms = static_cast<long>(remaining.count());
        if (remaining_ms <= 0) {
            return 0;  // 已超时。
        }
        if (remaining_ms > kMaxPollMs) {
            remaining_ms = kMaxPollMs;  // 分段等待，防 int 溢出。
        }
#ifdef _WIN32
        WSAPOLLFD pfd{};
        pfd.fd = socket.native_handle();
        pfd.events = POLLRDNORM;
        int ret = ::WSAPoll(&pfd, 1, static_cast<int>(remaining_ms));
        if (ret > 0) {
            return 1;
        }
        if (ret == 0) {
            continue;  // 本段超时：回循环重算剩余时间（deadline 未到则继续等）。
        }
        if (WSAGetLastError() != WSAEINTR) {
            return -1;
        }
#else
        pollfd pfd{};
        pfd.fd = socket.native_handle();
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, static_cast<int>(remaining_ms));
        if (ret > 0) {
            return 1;
        }
        if (ret == 0) {
            continue;  // 本段超时：回循环重算剩余时间。
        }
        if (errno != EINTR) {
            return -1;
        }
#endif
        // EINTR：信号打断系统调用，回循环按 deadline 重算剩余时间重试。
    }
}

} // namespace

Client::~Client() {
    disconnect();
}

bool Client::connect(std::string_view host, uint16_t port) {
    // 清理可能残留的旧连接（已连接再重连，或上次 read 失败后 socket 未关）：
    // 复用已打开的 socket 会直接连接失败，且残留缓冲会污染新连接的数据流。
    disconnect();

    asio::error_code ec;
    asio::ip::tcp::resolver resolver(io_context_);
    auto results = resolver.resolve(std::string(host), std::to_string(port), ec);
    if (ec) {
        return false;
    }
    asio::connect(socket_, results, ec);
    if (ec) {
        return false;
    }
    return true;
}

void Client::disconnect() {
    asio::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    read_buffer_.clear();
}

void Client::shutdown_send() {
    asio::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

bool Client::send_line(std::string_view line) {
    std::string msg(line);
    msg += '\n';
    asio::error_code ec;
    asio::write(socket_, asio::buffer(msg), ec);
    return !ec;
}

bool Client::send_raw(std::string_view data) {
    asio::error_code ec;
    asio::write(socket_, asio::buffer(data), ec);
    return !ec;
}

bool Client::read_line(std::string& out, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (extract_line(out)) {
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;  // 超时。
        }

        // 阻塞等待可读（内核态休眠，不忙等轮询）。剩余时间由 wait_readable
        // 按 deadline 内部计算（含 EINTR 重试与分段等待）。
        int wr = wait_readable(socket_, deadline);
        if (wr == 0) {
            return false;  // 超时（deadline 已到）。
        }
        if (wr < 0) {
            return false;  // poll 出错（fd 无效等，连接已不可用）。
        }

        asio::error_code ec;
        char buf[1024];
        std::size_t n = socket_.read_some(asio::buffer(buf), ec);
        if (ec || n == 0) {
            // 出错或对端优雅关闭（EOF）：连接已不可用，关闭 socket 让
            // is_open() 如实返回 false（避免"已断开但仍报告已连接"的误导）。
            asio::error_code ignored;
            socket_.close(ignored);
            read_buffer_.clear();
            return false;
        }
        // 缓冲上限：对端持续发送无换行数据时防止内存无限增长（异常对端）。
        if (read_buffer_.size() + n > kMaxReadBuffer) {
            asio::error_code ignored;
            socket_.close(ignored);
            read_buffer_.clear();
            return false;
        }
        read_buffer_.append(buf, n);
    }
}

bool Client::is_open() const {
    return socket_.is_open();
}

bool Client::extract_line(std::string& out) {
    auto pos = read_buffer_.find('\n');
    if (pos == std::string::npos) {
        return false;  // 尚无完整行。
    }
    out.assign(read_buffer_, 0, pos);
    read_buffer_.erase(0, pos + 1);
    if (!out.empty() && out.back() == '\r') {
        out.pop_back();  // 兼容 CRLF。
    }
    return true;
}

} // namespace sc
