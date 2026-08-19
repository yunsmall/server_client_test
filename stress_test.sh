#!/bin/bash
# 并发循环压测：多进程并行运行 gtest 测试，任一进程崩溃或断言失败即终止全部。
# 用法: stress_test.sh <可执行文件> <总次数> <进程数>
#   例: ./stress_test.sh ./build/server_client_stress_test.exe 2400 12   (Windows)
#        ./stress_test.sh ./build-linux/server_client_stress_test 2400 12  (Linux)
# 单进程运行次数 = ceil(总次数 / 进程数)，实际总次数可能略多于目标值。
# 不依赖调试器：测试进程自身安装崩溃处理器（test/crash_dump.hpp），崩溃或
# 断言失败时打印 "!!! CRASH" 标记与调用栈后终止。
# 并发度说明：连接风暴速率会超过系统端口池（TIME_WAIT 回收）容量时，
# 客户端 connect 失败属系统限制；建议进程数不超过 CPU 核数。
set -u
if [ $# -ne 3 ]; then
    echo "用法: $0 <可执行文件> <总次数> <进程数>" >&2
    exit 2
fi
exe=$1
total=$2
procs=$3
per=$(( (total + procs - 1) / procs ))  # 每进程次数（向上取整）

echo "目标: 总 $total 次，$procs 进程并发，每进程 $per 次"
rm -f stress_*.log
pids=""
for i in $(seq 1 "$procs"); do
    "$exe" --gtest_repeat="$per" --gtest_break_on_failure --gtest_brief=1 > "stress_$i.log" 2>&1 &
    pids="$pids $!"
done

# 轮询：任一进程崩溃（!!! CRASH）或测试失败（FAILED），立即终止其余进程。
while true; do
    any_alive=0
    for p in $pids; do
        if kill -0 "$p" 2>/dev/null; then
            any_alive=1
            break
        fi
    done
    [ "$any_alive" -eq 0 ] && break
    if grep -q "!!! CRASH\|FAILED" stress_*.log 2>/dev/null; then
        echo "!!! 检测到崩溃/失败，终止其余进程"
        # 先等崩溃进程打完自身栈（检测到标记时栈可能还在打印中）。
        sleep 5
        for p in $pids; do
            kill "$p" 2>/dev/null
        done
        sleep 2
        break
    fi
    sleep 0.5
done

echo "=== 崩溃现场（进程自打印的调用栈） ==="
grep -l "!!! CRASH" stress_*.log 2>/dev/null | while read f; do
    echo "--- $f ---"
    grep -A45 "!!! CRASH" "$f" | head -55
done
echo "=== 测试断言失败（FAILED）的进程 ==="
grep -l "FAILED" stress_*.log 2>/dev/null || echo "无"
echo "=== 全部完成 ==="
