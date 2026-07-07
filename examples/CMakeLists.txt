# libcoro 示例 —— 演示用安装后的 find_package 方式消费 libcoro。
#
#   # 先把 libcoro 装到某个 prefix:
#   cmake -B ../build -DLIBCORO_OS_BACKEND=epoll -DCMAKE_INSTALL_PREFIX=/tmp/lc
#   cmake --build ../build --target install
#   # 再单独构建示例:
#   cmake -B build -DCMAKE_PREFIX_PATH=/tmp/lc
#   cmake --build build && ./build/sleep_tasks
cmake_minimum_required(VERSION 3.16)
project(libcoro_examples C)

set(CMAKE_C_STANDARD 11)

find_package(libcoro REQUIRED)

add_executable(sleep_tasks sleep_tasks.c)
target_link_libraries(sleep_tasks PRIVATE libcoro::libcoro)
