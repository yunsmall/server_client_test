// 耗时用例：BLOCK 阻塞、超时等待、connect 重试。
// 这些用例不参与循环压测（耗时长且与竞态无关），只在完整测试里跑一次。

#include "test_common.hpp"

using namespace std::chrono_literals;

// BLOCK 阻塞隔离：A 阻塞时 B 应立即得到响应，不被拖累。
TEST_F(ServerClientTest, BlockIsolation) {
    auto a = make_client();
    ASSERT_NE(a, nullptr);

    // A 阻塞 2 秒。
    auto future_a = std::async(std::launch::async, [&] {
        std::string resp;
        bool ok = a->send_line("BLOCK 2") && a->read_line(resp);
        return std::make_pair(ok, resp);
    });

    // 等 A 进入阻塞。
    std::this_thread::sleep_for(100ms);

    // B 应立即得到响应。
    auto b = make_client();
    ASSERT_NE(b, nullptr);
    auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(b->send_line("PING"));
    std::string resp_b;
    ASSERT_TRUE(b->read_line(resp_b, 1s));
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(resp_b, "PONG");
    EXPECT_LT(elapsed, 1s);

    // A 最终收到 OK。
    auto [ok_a, resp_a] = future_a.get();
    EXPECT_TRUE(ok_a);
    EXPECT_EQ(resp_a, "OK");
}

// 多个客户端并发 BLOCK：总时长应约为单次 BLOCK 时长而非串行累加。
TEST_F(ServerClientTest, ConcurrentBlock) {
    constexpr int kClients = 10;
    std::vector<std::unique_ptr<sc::Client>> clients;
    clients.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        auto c = std::make_unique<sc::Client>();
        ASSERT_TRUE(c->connect("127.0.0.1", port_));
        clients.push_back(std::move(c));
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::future<std::string>> futures;
    futures.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        futures.push_back(std::async(std::launch::async, [&, i] {
            std::string resp;
            if (!clients[i]->send_line("BLOCK 1") || !clients[i]->read_line(resp)) {
                return std::string{};
            }
            return resp;
        }));
    }
    for (auto& f : futures) {
        EXPECT_EQ(f.get(), "OK");
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    // 10 个并发 BLOCK 1 秒，若非串行，总时长应远小于 10 秒。
    EXPECT_LT(elapsed, 3s);
}

// stop 优雅中止：应快速返回，并打断正在 BLOCK 的会话。
TEST(ServerStopTest, StopGracefulInterruptsBlock) {
    sc::Server server;
    server.start();
    uint16_t port = server.port();

    std::vector<std::unique_ptr<sc::Client>> clients;
    for (int i = 0; i < 3; ++i) {
        auto c = std::make_unique<sc::Client>();
        ASSERT_TRUE(c->connect("127.0.0.1", port));
        clients.push_back(std::move(c));
    }

    // 客户端 0 发 BLOCK 20 秒（应被 stop 打断）。
    std::atomic<bool> block_done{false};
    std::thread block_thread([&] {
        clients[0]->send_line("BLOCK 20");
        std::string resp;
        clients[0]->read_line(resp);  // socket 被关闭后立即返回 false。
        block_done = true;
    });

    std::this_thread::sleep_for(200ms);  // 让 BLOCK 进入阻塞。

    // stop 必须在 3 秒内返回（远小于 BLOCK 的 20 秒）。
    auto stop_future = std::async(std::launch::async, [&] { server.stop(); });
    auto status = stop_future.wait_for(3s);
    ASSERT_NE(status, std::future_status::timeout) << "stop() 未在 3 秒内返回";

    block_thread.join();
    EXPECT_TRUE(block_done.load());

    // 其余客户端也应读到 EOF。
    for (int i = 1; i < 3; ++i) {
        std::string resp;
        EXPECT_FALSE(clients[i]->read_line(resp, 1s));
    }
    EXPECT_EQ(server.active_connections(), 0);
}

// 服务端不响应时，read_line 应在超时后返回 false（验证 select 超时路径）。
TEST_F(ServerClientTest, ReadLineTimeout) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);

    std::string resp;
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(c->read_line(resp, 200ms));
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // 超时应约 200ms：不小于 150ms（确实等到了超时），远小于默认 5s。
    EXPECT_GE(elapsed, 150ms);
    EXPECT_LT(elapsed, 2s);
}

// 拆包：一行命令拆成多次发送，服务端应等完整行后才响应。
TEST_F(ServerClientTest, FragmentedLine) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);

    // 先发半行（无换行），服务端不应响应。
    ASSERT_TRUE(c->send_raw("PING"));
    std::string resp;
    EXPECT_FALSE(c->read_line(resp, 200ms));

    // 补上换行，服务端应完整解析并响应。
    ASSERT_TRUE(c->send_raw("\n"));
    ASSERT_TRUE(c->read_line(resp));
    EXPECT_EQ(resp, "PONG");
}

// connect 到已 stop 的服务器应失败（Windows 上 connect 未监听端口有 SYN 重试，耗时）。
TEST(ClientConnectTest, ConnectToStoppedServer) {
    sc::Server server;
    server.start();
    uint16_t port = server.port();
    server.stop();

    sc::Client c;
    EXPECT_FALSE(c.connect("127.0.0.1", port));
}

// 高连接数：300 个活跃连接同时保持，验证 poll/WSAPoll 无 fd 上限问题
// （select 在 FD_SETSIZE=1024 时越界，已改用 poll），并验证可正常通信。
// 容量类测试单次运行足够，不参与循环压测：高频重复会持续产生大量
// TIME_WAIT 连接，耗尽 Windows 动态端口池（16384 个，TIME_WAIT 存活 240 秒），
// 导致压测中的 connect 失败——这是系统端口池限制，与实现无关。
TEST(ServerCapacityTest, HighConnectionCount) {
    sc::Server server;
    server.start();
    uint16_t port = server.port();

    constexpr int kClients = 300;
    std::vector<std::unique_ptr<sc::Client>> clients;
    clients.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        auto c = std::make_unique<sc::Client>();
        ASSERT_TRUE(c->connect("127.0.0.1", port));
        clients.push_back(std::move(c));
    }
    EXPECT_TRUE(wait_until([&] { return server.active_connections() == kClients; }, 5s));

    // 抽查若干连接的通信正常。
    for (int i = 0; i < kClients; i += 50) {
        std::string resp;
        ASSERT_TRUE(clients[i]->send_line("PING"));
        ASSERT_TRUE(clients[i]->read_line(resp, 30s));
        EXPECT_EQ(resp, "PONG");
    }

    // 全部断开后服务端应清理完毕。
    clients.clear();
    EXPECT_TRUE(wait_until([&] { return server.active_connections() == 0; }, 5s));
    server.stop();
}
