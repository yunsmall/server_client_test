#include "server_client/protocol.hpp"

#include <cctype>
#include <limits>
#include <vector>

namespace sc {
namespace {

// 去除首尾空白。
std::string_view trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 按空白字符切分。
std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// 严格解析整数（整串必须全部被消费）。
bool parse_int(std::string_view s, long long& out) {
    std::string tmp(s);
    std::size_t pos = 0;
    try {
        out = std::stoll(tmp, &pos);
    } catch (...) {
        return false;
    }
    return pos == tmp.size();
}

} // namespace

Reply process_line(std::string_view line) {
    std::string_view s = trim(line);
    if (s.empty()) {
        return Reply{.text = std::string(protocol::kErrorUnknown)};
    }

    // 提取命令名（第一个空白字符之前，兼容 tab 等任意空白）与参数。
    std::size_t sp = 0;
    while (sp < s.size() && !std::isspace(static_cast<unsigned char>(s[sp]))) {
        ++sp;
    }
    std::string_view cmd = s.substr(0, sp);
    // 参数为命令名之后、跳过一个分隔空白字符的剩余内容（整行首尾空白已在
    // trim 去掉）：ECHO 按此精确回显，其余命令经 split_ws 自行处理空白，
    // 无需在此先行裁剪。
    std::string_view args = (sp == s.size()) ? std::string_view{} : s.substr(sp + 1);

    if (cmd == "PING") {
        return Reply{.text = std::string(protocol::kPong)};
    }

    if (cmd == "ECHO") {
        // 回显命令名之后、行尾之前的原始内容（跳过分隔空白字符，行首行尾
        // 空白已在 trim 去掉，命令名与参数之间的其余空白原样保留）。
        return Reply{.text = std::string(args)};
    }

    if (cmd == "ADD") {
        auto toks = split_ws(args);
        if (toks.size() != 2) {
            return Reply{.text = std::string(protocol::kErrorAdd)};
        }
        long long a = 0;
        long long b = 0;
        if (!parse_int(toks[0], a) || !parse_int(toks[1], b)) {
            return Reply{.text = std::string(protocol::kErrorAdd)};
        }
        // 溢出检查：避免 a+b 有符号溢出（未定义行为）。
        if ((b > 0 && a > std::numeric_limits<long long>::max() - b) ||
            (b < 0 && a < std::numeric_limits<long long>::min() - b)) {
            return Reply{.text = std::string(protocol::kErrorAdd)};
        }
        return Reply{.text = std::to_string(a + b)};
    }

    if (cmd == "BLOCK") {
        auto toks = split_ws(args);
        if (toks.size() != 1) {
            return Reply{.text = std::string(protocol::kErrorBlock)};
        }
        long long n = 0;
        if (!parse_int(toks[0], n) || n <= 0) {
            return Reply{.text = std::string(protocol::kErrorBlock)};
        }
        // 上限检查：block_seconds 为 int，超出即视为非法，避免 static_cast 有符号溢出。
        if (n > std::numeric_limits<int>::max()) {
            return Reply{.text = std::string(protocol::kErrorBlock)};
        }
        return Reply{.text = std::string(protocol::kOk), .block_seconds = static_cast<int>(n)};
    }

    if (cmd == "QUIT") {
        // 断连命令：直接关闭连接，不回响应（客户端以读 EOF 感知断开）。
        return Reply{.close_after = true};
    }

    return Reply{.text = std::string(protocol::kErrorUnknown)};
}

} // namespace sc
