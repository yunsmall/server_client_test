#pragma once

// 崩溃处理器：进程崩溃（访问违例、断言失败等）时用 C++23 标准的 <stacktrace>
// 打印自身调用栈并终止，供压测摆脱 gdb 包裹（压测脚本按输出中的
// "!!! CRASH" 标记检测崩溃）。
// 平台差异：Linux 用信号处理器（gtest 断言失败 raise(SIGTRAP) 一并捕获）；
// Windows 用 SetUnhandledExceptionFilter（未处理的结构化异常，含断言失败
// 触发的 DebugBreak 断点）与 CRT 的 SIGABRT。
// 工具链要求：Linux 需 g++-13+ 并链接 stdc++_libbacktrace（gcc-14 的
// Ubuntu PPA 缺该库，13 自带）；Windows 需 VS2022 17.5+（MSVC STL）。

#include <stacktrace>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace crash_dump {

namespace {

// 打印栈（逐帧：函数 + 文件:行号）。
void print_stack(const std::stacktrace& st) {
    std::fprintf(stderr, "=== CRASH STACKTRACE (%zu frames) ===\n",
                 static_cast<std::size_t>(st.size()));
    std::size_t i = 0;
    for (const auto& e : st) {
        std::fprintf(stderr, "  #%-2zu %s", i++, e.description().c_str());
        const auto& file = e.source_file();
        if (!file.empty()) {
            std::fprintf(stderr, "  at %s:%lu", file.c_str(),
                         static_cast<unsigned long>(e.source_line()));
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

#ifdef _WIN32
// 未处理异常过滤器：打印异常代码与调用栈后返回 EXCEPTION_EXECUTE_HANDLER，
// 让进程按未处理异常终止（退出码非零，压测脚本据此检测）。
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "\n!!! CRASH: exception code 0x%08lX\n",
                 static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode));
    print_stack(std::stacktrace::current());
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
// 信号处理器：恢复默认动作防递归，打印后退出。
void crash_handler(int sig) {
    std::signal(sig, SIG_DFL);
    std::fprintf(stderr, "\n!!! CRASH: signal %d\n", sig);
    print_stack(std::stacktrace::current());
    _exit(1);
}
#endif

} // namespace

// 安装崩溃处理器（在 gtest 初始化前调用）。
inline void install() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    // CRT 的 abort() 不走未处理异常过滤器，单独用信号捕获。
    std::signal(SIGABRT, [](int) {
        std::fprintf(stderr, "\n!!! CRASH: SIGABRT\n");
        print_stack(std::stacktrace::current());
        _exit(3);
    });
#else
    struct sigaction sa {};
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) {
        sigaction(sig, &sa, nullptr);
    }
#endif
}

} // namespace crash_dump
