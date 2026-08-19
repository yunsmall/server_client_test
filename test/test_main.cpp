// 测试统一入口：安装崩溃处理器（崩溃/断言失败时打印自身栈），再启动 gtest。
// gtest_main 不提供崩溃打印，压测脚本需要按输出中的 "!!! CRASH" 标记检测崩溃，
// 因此这里自定义 main。

#include "crash_dump.hpp"

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    // 在 gtest 初始化前安装：断言失败（Windows DebugBreak / Linux SIGTRAP）
    // 也走未处理异常路径，由处理器打印栈并终止进程。
    crash_dump::install();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
