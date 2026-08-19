#pragma once

#include <asio.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sc {

// TCP 客户端，使用同步 API。
// 具备主动断开能力（disconnect），并支持带超时的同步按行读取。
// 线程模型：单个 Client 对象应由单个线程使用（asio 共享对象并发访问为未定义
// 行为）。disconnect() 的"可打断阻塞读"仅指同一线程内先发出阻塞读、超时后
// 再调用 disconnect 收尾；从其他线程打断阻塞读属于跨线程并发访问 socket，
// 不在本库保证范围内。
class Client {
public:
    Client() = default;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // 连接服务端，失败返回 false。若已存在旧连接，会先关闭旧连接再重连。
    bool connect(std::string_view host, uint16_t port);

    // 主动断开连接：shutdown + close，可打断阻塞中的读取。
    void disconnect();

    // 半关闭：只关闭发送方向（向对端发 FIN），仍可继续读取对端发来的数据。
    void shutdown_send();

    // 发送一行文本（自动补 '\n'），失败返回 false。
    bool send_line(std::string_view line);

    // 发送原始字节（不自动补 '\n'），用于拆包、半行等边界场景。
    bool send_raw(std::string_view data);

    // 同步读取一行（去掉行尾 '\n'，兼容 '\r\n'）。
    // 在 timeout 内没有完整行、连接关闭或出错时返回 false。
    bool read_line(std::string& out, std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // 当前是否处于已连接状态。
    bool is_open() const;

private:
    // 从内部缓冲提取一行，成功返回 true。
    bool extract_line(std::string& out);

    // 未按行交付的最大缓冲字节数：对端持续发送无换行数据时超出即断开，
    // 防止内存无限增长。
    static constexpr std::size_t kMaxReadBuffer = 1024 * 1024;

    asio::io_context io_context_;
    asio::ip::tcp::socket socket_{io_context_};
    std::string read_buffer_;  // 已从 socket 读入、尚未按行交付的字节
};

} // namespace sc
