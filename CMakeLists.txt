cmake_minimum_required(VERSION 4.0)
project(libcoro VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

set(LIBCORO_OS_BACKEND "" CACHE STRING "Choose OS backend: epoll, select, win, lwip")
set_property(CACHE LIBCORO_OS_BACKEND PROPERTY STRINGS epoll select win lwip)

add_library(libcoro)

list(APPEND SRC_FILES
        src/future.c
        src/generator.c
        src/task.c
        src/offload.c
        include/private/loop.h
        include/private/future.h
        include/private/generator.h
        include/private/task.h
        include/private/pal_thread.h
        include/private/offload.h
)

if(LIBCORO_OS_BACKEND STREQUAL "epoll")
    list(APPEND SRC_FILES src/loop/linux_epoll.c src/pal/thread_posix.c)
elseif(LIBCORO_OS_BACKEND STREQUAL "select")
    list(APPEND SRC_FILES src/loop/linux_select.c src/pal/thread_posix.c)

elseif(LIBCORO_OS_BACKEND STREQUAL "win")
    list(APPEND SRC_FILES src/loop/windows.c src/pal/thread_win.c)
elseif(LIBCORO_OS_BACKEND STREQUAL "lwip")
    # lwIP 后端 —— 阶段 1: UNIX host port (socket 模式, 底层仍 pthread)。
    # loop 事件循环用 lwip_select; 线程原语沿用 thread_posix (offload 保留)。
    list(APPEND SRC_FILES src/loop/lwip_select.c src/pal/thread_posix.c)

    # 拉 lwIP 源码 (可用 FETCHCONTENT_SOURCE_DIR_LWIP 指向本地 clone 免网络)。
    # SOURCE_SUBDIR 指向一个没有 CMakeLists.txt 的子目录 => MakeAvailable 只
    # 下载、跳过 add_subdirectory: 我们不想跑 lwIP 自己的 CMakeLists (会拉
    # doxygen/example_app, 还会提前触发 Filelists 的 include_guard(GLOBAL),
    # 使后面我们 include() 拿 *_SRCS 变 no-op)。我们只要它的源码路径。
    include(FetchContent)
    FetchContent_Declare(
            lwip
            GIT_REPOSITORY https://github.com/lwip-tcpip/lwip.git
            GIT_TAG master
            SOURCE_SUBDIR  do-not-add-subdir
    )
    FetchContent_MakeAvailable(lwip)

    # lwIP 的 Filelists.cmake 是拿来 include() 的: 它假设 LWIP_DIR /
    # LWIP_CONTRIB_DIR 已定义, 并读 LWIP_INCLUDE_DIRS 作为编译 include 路径,
    # 然后暴露 *_SRCS 变量供我们自己建库。
    set(LWIP_DIR         ${lwip_SOURCE_DIR})
    set(LWIP_CONTRIB_DIR ${lwip_SOURCE_DIR}/contrib)
    set(LWIP_INCLUDE_DIRS
            ${CMAKE_CURRENT_SOURCE_DIR}/ports/lwip                       # 我们的 lwipopts.h
            ${LWIP_DIR}/src/include
            ${LWIP_CONTRIB_DIR}/ports/unix/port/include                  # arch/*.h, sys_arch.h
    )
    include(${LWIP_DIR}/src/Filelists.cmake)
    include(${LWIP_CONTRIB_DIR}/ports/unix/Filelists.cmake)

    # 建一个 lwip 静态库: 核心 + IPv4/6 + api + netif + unix port sys_arch。
    add_library(lwip_backend STATIC
            ${lwipcore_SRCS} ${lwipcore4_SRCS} ${lwipcore6_SRCS}
            ${lwipapi_SRCS}  ${lwipnetif_SRCS}
            ${LWIP_CONTRIB_DIR}/ports/unix/port/sys_arch.c
    )
    target_include_directories(lwip_backend PUBLIC ${LWIP_INCLUDE_DIRS})
    find_package(Threads REQUIRED)
    target_link_libraries(lwip_backend PUBLIC Threads::Threads)

    set(LIBCORO_LWIP 1)
else()
    message(FATAL_ERROR "No valid OS backend selected. Please choose one of: epoll, select, win, lwip")
endif()

target_sources(libcoro PRIVATE ${SRC_FILES})

target_include_directories(libcoro
        PUBLIC
        include/public
        PRIVATE
        include/private
)

if(WIN32)
    target_link_libraries(libcoro PRIVATE ws2_32)
else()
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(libcoro PUBLIC Threads::Threads)
endif()

if(LIBCORO_LWIP)
    # lwip_select.c 要 include lwIP 头; libcoro 的使用方 (asyncweb) 通过
    # PUBLIC 传递也能拿到 lwip_ socket API。
    target_link_libraries(libcoro PUBLIC lwip_backend)
    target_compile_definitions(libcoro PUBLIC LIBCORO_LWIP=1)
endif()

if(LIBCORO_LWIP)
    # lwip 后端: test_loop (系统 socket + 外部网络) 换成纯栈内环回 echo。
    set(LIBCORO_LOOP_TEST tests/test_lwip_echo.c)
else()
    set(LIBCORO_LOOP_TEST tests/test_loop.c)
endif()

add_executable(libcoro_tests
        tests/main.c
        ${LIBCORO_LOOP_TEST}
        tests/test_nested_task.c
        tests/test_empty_task.c
        tests/test_offload.c
)
target_link_libraries(libcoro_tests PRIVATE libcoro)

enable_testing()
if(LIBCORO_LWIP)
    add_test(NAME test_lwip_echo COMMAND libcoro_tests lwip_echo)
else()
    add_test(NAME test_loop COMMAND libcoro_tests loop)
endif()
add_test(NAME test_nested_task COMMAND libcoro_tests nested_task)
add_test(NAME test_empty_task COMMAND libcoro_tests empty_task)
add_test(NAME test_offload COMMAND libcoro_tests offload)
