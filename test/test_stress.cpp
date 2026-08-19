// 压测用例：并发、竞态、压力、连接生命周期等，供循环压测直接运行。
// 命令功能测试见 test_protocol.cpp，耗时用例（BLOCK、超时、connect 重试）见 test_slow.cpp。

#include "test_common.hpp"

#include <cstdio>

using namespace std::chrono_literals;

// 清理断言统一用 10s 余量：20 进程并发压测时系统负载高，连接清理可能被
// 拖慢，3s 在负载尖峰下会误报；真有残留（10s 仍不成立）依然会失败。
// 多客户端并发：响应必须与发送方一一对应、互不串扰。
TEST_F(ServerClientTest, MultiClientConcurrent) {
    constexpr int kClients = 50;
    constexpr int kRounds = 10;
    std::atomic<int> connect_fail{0};  // 连接失败数
    std::atomic<int> io_fail{0};       // 收发失败数（含超时）
    std::atomic<int> mismatch{0};      // 响应内容与发送方不匹配数（串扰）
    std::vector<std::thread> threads;
    threads.reserve(kClients);

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([this, i, &connect_fail, &io_fail, &mismatch] {
            sc::Client c;
            if (!c.connect("127.0.0.1", port_)) {
                ++connect_fail;
                return;
            }
            for (int j = 0; j < kRounds; ++j) {
                std::string echo_msg = "client" + std::to_string(i) + "-" + std::to_string(j);
                std::string resp;
                if (!c.send_line("ECHO " + echo_msg)) {
                    ++io_fail;
                    return;
                }
                if (!c.read_line(resp, 30s)) {
                    ++io_fail;
                    return;
                }
                if (resp != echo_msg) {
                    ++mismatch;
                    return;
                }
                if (!c.send_line("ADD " + std::to_string(i) + " " + std::to_string(j))) {
                    ++io_fail;
                    return;
                }
                if (!c.read_line(resp, 30s)) {
                    ++io_fail;
                    return;
                }
                if (resp != std::to_string(i + j)) {
                    ++mismatch;
                    return;
                }
            }
            c.disconnect();
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(connect_fail.load(), 0);
    EXPECT_EQ(io_fail.load(), 0);
    EXPECT_EQ(mismatch.load(), 0);
    EXPECT_TRUE(wait_until([this] { return server_->active_connections() == 0; }, 10s));
}

// 客户端主动断开：服务端应及时移除该连接，其余客户端不受影响。
TEST_F(ServerClientTest, ClientActiveDisconnect) {
    auto a = make_client();
    auto b = make_client();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_TRUE(wait_until([this] { return server_->active_connections() == 2; }, 10s));

    a->disconnect();

    EXPECT_TRUE(wait_until([this] { return server_->active_connections() == 1; }, 10s));

    std::string resp;
    ASSERT_TRUE(b->send_line("PING"));
    ASSERT_TRUE(b->read_line(resp, 30s));
    EXPECT_EQ(resp, "PONG");
}

// 大量客户端连接后立即断开（未发送任何数据），服务端应能正确移除全部连接。
TEST_F(ServerClientTest, ConnectThenImmediateDisconnect) {
    for (int i = 0; i < 100; ++i) {
        auto c = make_client();
        ASSERT_NE(c, nullptr);
        // 构造即连接，作用域结束即析构断开，未发送任何数据。
    }
    EXPECT_TRUE(wait_until([this] { return server_->active_connections() == 0; }, 10s));
}

// 连接后不发任何数据，服务端应保持连接，且不影响其他客户端。
TEST_F(ServerClientTest, ConnectThenSilent) {
    auto silent = make_client();
    ASSERT_NE(silent, nullptr);
    // silent 不发数据，保持连接。

    auto b = make_client();
    ASSERT_NE(b, nullptr);
    std::string resp;
    ASSERT_TRUE(b->send_line("PING"));
    ASSERT_TRUE(b->read_line(resp, 30s));
    EXPECT_EQ(resp, "PONG");

    EXPECT_EQ(server_->active_connections(), 2);
}

// 半关闭：客户端关闭发送方向后，服务端读到 EOF 并关闭连接。
TEST_F(ServerClientTest, HalfClose) {
    auto c = make_client();
    ASSERT_NE(c, nullptr);

    std::string resp;
    ASSERT_TRUE(c->send_line("PING"));
    ASSERT_TRUE(c->read_line(resp, 30s));
    EXPECT_EQ(resp, "PONG");

    c->shutdown_send();

    // 服务端读到 EOF 会关闭连接，客户端应读到 EOF。
    EXPECT_FALSE(c->read_line(resp, 1s));

    EXPECT_TRUE(wait_until([this] { return server_->active_connections() == 0; }, 10s));
}

// disconnect 幂等、disconnect 后收发失败、Client 对象复用重新连接。
TEST(ClientConnectTest, ReconnectAfterDisconnect) {
    sc::Server server;
    server.start();
    uint16_t port = server.port();

    sc::Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port));

    c.disconnect();
    c.disconnect();  // 幂等。

    // disconnect 后收发均失败。
    EXPECT_FALSE(c.send_line("PING"));
    std::string resp;
    EXPECT_FALSE(c.read_line(resp, 200ms));

    // 复用同一 Client 对象重新连接。
    ASSERT_TRUE(c.connect("127.0.0.1", port));
    ASSERT_TRUE(c.send_line("PING"));
    ASSERT_TRUE(c.read_line(resp, 30s));
    EXPECT_EQ(resp, "PONG");

    server.stop();
}

// stop 后 start 重启：可反复多次，每次重启后都能正常服务新连接。
TEST(ServerRestartTest, StopStartCycle) {
    sc::Server server;
    for (int round = 0; round < 10; ++round) {
        server.start();
        uint16_t port = server.port();

        sc::Client c;
        ASSERT_TRUE(c.connect("127.0.0.1", port));
        std::string resp;
        ASSERT_TRUE(c.send_line("PING"));
        ASSERT_TRUE(c.read_line(resp, 30s));
        EXPECT_EQ(resp, "PONG");

        server.stop();

        // stop 后连接已被服务端关闭。
        EXPECT_FALSE(c.read_line(resp, 500ms));
    }
}

// 压力测试：大量客户端并发大量收发。
TEST(ServerStressTest, ConcurrentBurst) {
    sc::Server server;
    server.start();
    uint16_t port = server.port();

    constexpr int kClients = 20;
    constexpr int kRounds = 100;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kClients);

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            sc::Client c;
            if (!c.connect("127.0.0.1", port)) {
                ++failures;
                return;
            }
            for (int j = 0; j < kRounds; ++j) {
                std::string echo_msg = "stress-" + std::to_string(i) + "-" + std::to_string(j);
                std::string resp;
                if (!c.send_line("ECHO " + echo_msg) || !c.read_line(resp, 30s) ||
                    resp != echo_msg) {
                    ++failures;
                    return;
                }
                if (!c.send_line("ADD " + std::to_string(i) + " " + std::to_string(j)) ||
                    !c.read_line(resp, 30s) || resp != std::to_string(i + j)) {
                    ++failures;
                    return;
                }
            }
            c.disconnect();
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(failures.load(), 0);
    server.stop();
}

// 随机断开重连：客户端反复连接、收发、主动断开。
TEST(ServerStressTest, DisconnectReconnect) {
    sc::Server server;
    server.start();
    uint16_t port = server.port();

    constexpr int kClients = 10;
    constexpr int kCycles = 20;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kClients);

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            for (int cyc = 0; cyc < kCycles; ++cyc) {
                sc::Client c;
                if (!c.connect("127.0.0.1", port)) {
                    ++failures;
                    std::fprintf(stderr, "!!! connect 失败（客户端 %d 循环 %d）\n", i, cyc);
                    return;
                }
                std::string resp;
                std::string echo_msg = "cycle-" + std::to_string(i) + "-" + std::to_string(cyc);
                if (!c.send_line("ECHO " + echo_msg) || !c.read_line(resp, 30s) ||
                    resp != echo_msg) {
                    ++failures;
                    std::fprintf(stderr, "!!! ECHO 失败（客户端 %d 循环 %d）\n", i, cyc);
                    return;
                }
                c.disconnect();  // 主动断开。
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(failures.load(), 0);
    server.stop();
}

// stop/start 幂等：空服务端 stop、重复 stop、重复 start 均应安全。
TEST(ServerStopTest, IdempotentStopStart) {
    sc::Server server;

    server.start();
    server.start();  // 重复 start 应为无操作。

    server.stop();
    server.stop();   // 重复 stop 应为无操作。

    // 空服务端 stop 后再次 start，仍能正常工作。
    server.start();
    uint16_t port = server.port();
    sc::Client c;
    ASSERT_TRUE(c.connect("127.0.0.1", port));
    std::string resp;
    ASSERT_TRUE(c.send_line("PING"));
    ASSERT_TRUE(c.read_line(resp, 30s));
    EXPECT_EQ(resp, "PONG");
    server.stop();
    server.stop();  // 再次幂等 stop。
}

// 大量活跃连接（未通信）时 stop 应快速返回，并关闭所有连接。
TEST(ServerStopTest, StopWithManyActiveClients) {
    sc::Server server;
    server.start();
    uint16_t port = server.port();

    constexpr int kClients = 50;
    std::vector<std::unique_ptr<sc::Client>> clients;
    clients.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        auto c = std::make_unique<sc::Client>();
        ASSERT_TRUE(c->connect("127.0.0.1", port));
        clients.push_back(std::move(c));
    }

    auto stop_future = std::async(std::launch::async, [&] { server.stop(); });
    auto status = stop_future.wait_for(10s);
    ASSERT_NE(status, std::future_status::timeout) << "stop() 未在 10 秒内返回";

    // 所有客户端连接应已被服务端关闭。
    for (auto& c : clients) {
        std::string resp;
        EXPECT_FALSE(c->read_line(resp, 500ms));
    }
    EXPECT_EQ(server.active_connections(), 0);
}

// stop 与 accept 的竞争：大量连接正在建立时调用 stop，
// 覆盖 accept 刚接受连接、尚未启动 Session 线程的竞争窗口。
TEST(ServerStopTest, StopDuringConnectRace) {
    constexpr int kIterations = 5;
    constexpr int kClients = 24;

    for (int iter = 0; iter < kIterations; ++iter) {
        sc::Server server;
        server.start();
        uint16_t port = server.port();

        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        threads.reserve(kClients);
        for (int i = 0; i < kClients; ++i) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                sc::Client c;
                // 连接可能成功，也可能因服务端停止而失败，两者均属正常。
                (void)c.connect("127.0.0.1", port);
                // 不主动断开，交由服务端 stop 统一清理。
            });
        }
        go.store(true, std::memory_order_release);

        // 让连接请求尽量同时涌入，制造 accept 与 stop 的竞争窗口。
        std::this_thread::sleep_for(2ms);
        server.stop();

        for (auto& t : threads) {
            t.join();
        }
        EXPECT_EQ(server.active_connections(), 0);
    }
}

// stop 与活跃 I/O 竞争：stop 时客户端正在并发收发，
// 覆盖读线程被 shutdown 打断、写队列清空、BLOCK 等待唤醒等收尾路径。
TEST(ServerStopTest, StopDuringActiveIO) {
    constexpr int kIterations = 5;
    constexpr int kClients = 10;

    for (int iter = 0; iter < kIterations; ++iter) {
        sc::Server server;
        server.start();
        uint16_t port = server.port();

        std::vector<std::thread> threads;
        threads.reserve(kClients);
        for (int i = 0; i < kClients; ++i) {
            threads.emplace_back([&, i] {
                sc::Client c;
                if (!c.connect("127.0.0.1", port)) {
                    return;
                }
                // 疯狂收发（ECHO 仅作负载），直到 stop 打断连接读写失败。
                for (int j = 0;; ++j) {
                    std::string resp;
                    if (!c.send_line("ECHO io-" + std::to_string(i) + "-" + std::to_string(j)) ||
                        !c.read_line(resp, 30s)) {
                        break;  // stop 关闭连接后读写失败，正常退出。
                    }
                }
            });
        }

        std::this_thread::sleep_for(10ms);  // 让客户端进入收发循环。
        auto stop_future = std::async(std::launch::async, [&] { server.stop(); });
        ASSERT_NE(stop_future.wait_for(10s), std::future_status::timeout)
            << "stop() 未在 10 秒内返回";

        for (auto& t : threads) {
            t.join();
        }
        EXPECT_EQ(server.active_connections(), 0);
    }
}
