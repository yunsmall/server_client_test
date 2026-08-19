cmake_minimum_required(VERSION 3.20)
project(server_client_test LANGUAGES CXX)

# 使用 C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vcpkg 提供的 standalone asio（头文件库，自动带 Threads::Threads）
find_package(asio CONFIG REQUIRED)
# vcpkg 提供的 gtest
find_package(GTest CONFIG REQUIRED)

# 核心库：协议 + 连接 + 服务端 + 客户端
add_library(server_client
    src/protocol.cpp
    src/session.cpp
    src/server.cpp
    src/client.cpp
)
target_include_directories(server_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(server_client PUBLIC asio::asio)
target_compile_features(server_client PUBLIC cxx_std_23)

# gtest 测试可执行文件
enable_testing()

# 完整测试：压测 + 耗时用例
add_executable(server_client_test
    test/test_main.cpp
    test/test_stress.cpp
    test/test_slow.cpp
    test/test_protocol.cpp
)
target_link_libraries(server_client_test PRIVATE
    server_client
    GTest::gtest
)
target_compile_features(server_client_test PRIVATE cxx_std_23)
add_test(NAME server_client_test COMMAND server_client_test)

# 压测专用：仅快速压测用例，供循环压测直接运行（不含耗时用例）
add_executable(server_client_stress_test
    test/test_main.cpp
    test/test_stress.cpp
)
target_link_libraries(server_client_stress_test PRIVATE
    server_client
    GTest::gtest
)
target_compile_features(server_client_stress_test PRIVATE cxx_std_23)
add_test(NAME server_client_stress_test COMMAND server_client_stress_test)

# 崩溃打印用 C++23 <stacktrace>：Linux 的 libstdc++ 把它放在
# libstdc++exp 里（g++-13 及以上链接 -lstdc++exp 即可）。
# Windows 的 MSVC STL 无需额外链接。
if(UNIX)
    target_link_libraries(server_client_test PRIVATE stdc++exp)
    target_link_libraries(server_client_stress_test PRIVATE stdc++exp)
endif()
