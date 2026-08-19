#pragma once

#include <string>
#include <string_view>

namespace sc {

// 一条客户端命令解析/处理后的结果。
struct Reply {
    std::string text;          // 响应正文（一行，不含换行，由发送方补 '\n'）
    bool close_after = false;  // 响应后服务端主动关闭该连接（QUIT 命令）
    int block_seconds = 0;     // >0 表示服务端在读线程中阻塞该秒数（BLOCK 命令）
};

// 解析一行命令文本，生成响应。
// 输入应为去除行尾换行后的单行文本。
Reply process_line(std::string_view line);

// 协议相关的常量文本，供实现与测试统一引用。
namespace protocol {
    inline constexpr std::string_view kPong = "PONG";
    inline constexpr std::string_view kOk = "OK";
    inline constexpr std::string_view kErrorUnknown = "ERROR unknown command";
    inline constexpr std::string_view kErrorAdd = "ERROR invalid ADD operands";
    inline constexpr std::string_view kErrorBlock = "ERROR invalid BLOCK duration";
} // namespace protocol

} // namespace sc
