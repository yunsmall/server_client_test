// 协议/命令功能测试：验证客户端发来的命令能被服务端正确解析与响应。
// 命令仅用于模拟负载，这些用例不参与循环压测。

#include "test_common.hpp"

using namespace std::chrono_literals;

// 基本协议命令。
TEST_F(ServerClientTest, BasicProtocol) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);

    std::string resp;

    ASSERT_TRUE(c->send_line("PING"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "PONG");

    ASSERT_TRUE(c->send_line("ECHO hello world"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "hello world");

    // tab 分隔也应正常解析（命令名按任意空白字符分割）。
    ASSERT_TRUE(c->send_line("ECHO\tfoo"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "foo");

    // ECHO 无参数回空行（协议保证每条命令都有响应，QUIT 除外）。
    ASSERT_TRUE(c->send_line("ECHO"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "");

    ASSERT_TRUE(c->send_line("ADD 2 3"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "5");

    ASSERT_TRUE(c->send_line("ADD -5 10"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "5");

    ASSERT_TRUE(c->send_line("UNKNOWN_CMD"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR unknown command");

    // QUIT：服务端直接关闭连接，无响应，客户端以读 EOF 感知断开。
    ASSERT_TRUE(c->send_line("QUIT"));
    EXPECT_FALSE(c->read_line(resp, 1s));
}

// 非法命令参数应返回明确错误，且不影响连接后续使用。
TEST_F(ServerClientTest, InvalidArguments) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);
    std::string resp;

    ASSERT_TRUE(c->send_line("ADD 1"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR invalid ADD operands");

    ASSERT_TRUE(c->send_line("ADD a b"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR invalid ADD operands");

    ASSERT_TRUE(c->send_line("BLOCK 0"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR invalid BLOCK duration");

    ASSERT_TRUE(c->send_line("BLOCK -1"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR invalid BLOCK duration");

    // 出错后连接仍可用。
    ASSERT_TRUE(c->send_line("PING"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "PONG");
}

// 超大整数参数不应触发溢出，应返回明确错误且连接保持可用。
TEST_F(ServerClientTest, IntegerOverflow) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);
    std::string resp;

    ASSERT_TRUE(c->send_line("ADD 9223372036854775807 1"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR invalid ADD operands");

    ASSERT_TRUE(c->send_line("ADD -9223372036854775808 -1"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR invalid ADD operands");

    ASSERT_TRUE(c->send_line("BLOCK 3000000000"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR invalid BLOCK duration");

    // 出错后连接仍可用。
    ASSERT_TRUE(c->send_line("PING"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "PONG");
}

// 空行应返回未知命令错误。
TEST_F(ServerClientTest, EmptyLine) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->send_line(""));
    std::string resp;
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "ERROR unknown command");
}

// 单行长度远超 streambuf 默认容量的长命令，read_until 应自动扩容并正确回显。
TEST_F(ServerClientTest, LongLine) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);

    std::string long_text(8 * 1024, 'x');
    ASSERT_TRUE(c->send_line("ECHO " + long_text));
    std::string resp;
    ASSERT_TRUE(c->read_line(resp, 5s));
    EXPECT_EQ(resp, long_text);
}

// 管道化：连续发送多条命令后再逐条读取，验证客户端缓冲能正确处理粘包。
TEST_F(ServerClientTest, PipelinedRead) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);

    constexpr int kMsgs = 200;
    for (int i = 0; i < kMsgs; ++i) {
        ASSERT_TRUE(c->send_line("ADD " + std::to_string(i) + " 0"));
    }
    for (int i = 0; i < kMsgs; ++i) {
        std::string resp;
        ASSERT_TRUE(c->read_line(resp, 5s));
        EXPECT_EQ(resp, std::to_string(i));
    }
}
