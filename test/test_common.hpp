#pragma once

#include <gtest/gtest.h>

#include "server_client/client.hpp"
#include "server_client/protocol.hpp"
#include "server_client/server.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// 轮询等待谓词成立，超时返回 false。
inline bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// 每个用例启动一个随机端口服务端。
class ServerClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<sc::Server>();
        server_->start();
        port_ = server_->port();
    }

    void TearDown() override {
        server_->stop();
    }

    std::unique_ptr<sc::Client> make_client() {
        auto c = std::make_unique<sc::Client>();
        if (!c->connect("127.0.0.1", port_)) {
            return nullptr;
        }
        return c;
    }

    std::unique_ptr<sc::Server> server_;
    uint16_t port_ = 0;
};
