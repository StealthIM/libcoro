cmake_minimum_required(VERSION 4.0)
project(libcoro VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

set(LIBCORO_OS_BACKEND "" CACHE STRING "Choose OS backend: epoll, select, win, lwip")
set_property(CACHE LIBCORO_OS_BACKEND PROPERTY STRINGS epoll select win lwip)

# offload 线程池: 有 OS 线程的目标默认开; 无线程目标 (裸机 NO_SYS) 应关。
# 关掉时 offload.c/pal_thread 不编, 换 offload_stub.c (全 no-op, loop_get_offload
# 返 NULL, loop_run_in_thread 返 NULL —— 调用方需自备无线程回落如 native-async DNS)。
option(LIBCORO_HAVE_OFFLOAD "Build the offload thread pool (requires OS threads)" ON)

add_library(libcoro)

list(APPEND SRC_FILES
        src/future.c
        src/generator.c
        src/task.c
        include/libcoro/libcoro.h
        include/libcoro/loop.h
        include/libcoro/future.h
        include/libcoro/generator.h
        include/libcoro/task.h
        include/libcoro/offload.h
        include/internal/loop_internal.h
        include/internal/offload_internal.h
        include/internal/pal_thread.h
)

# offload 能力: 开 => 真线程池 (offload.c) + 后端各自的 pal_thread;
#               关 => 空实现 (offload_stub.c), 不链任何线程原语。
if(LIBCORO_HAVE_OFFLOAD)
    list(APPEND SRC_FILES src/offload.c)
else()
    list(APPEND SRC_FILES src/offload_stub.c)
endif()

# pal_thread 只在开 offload 时按后端选 (关掉时无线程依赖)。
set(LIBCORO_PAL_THREAD "")
if(LIBCORO_HAVE_OFFLOAD)
    if(LIBCORO_OS_BACKEND STREQUAL "win")
        set(LIBCORO_PAL_THREAD src/pal/thread_win.c)
    else()
        set(LIBCORO_PAL_THREAD src/pal/thread_posix.c)
    endif()
endif()

if(LIBCORO_OS_BACKEND STREQUAL "epoll")
    list(APPEND SRC_FILES src/loop/linux_epoll.c ${LIBCORO_PAL_THREAD})
elseif(LIBCORO_OS_BACKEND STREQUAL "select")
    list(APPEND SRC_FILES src/loop/linux_select.c ${LIBCORO_PAL_THREAD})

elseif(LIBCORO_OS_BACKEND STREQUAL "win")
    list(APPEND SRC_FILES src/loop/windows.c ${LIBCORO_PAL_THREAD})
elseif(LIBCORO_OS_BACKEND STREQUAL "lwip")
    # lwIP 后端 —— 阶段 1: UNIX host port (socket 模式, 底层仍 pthread)。
    # loop 事件循环用 lwip_select; 线程原语沿用 thread_posix (offload 保留)。
    list(APPEND SRC_FILES src/loop/lwip_select.c ${LIBCORO_PAL_THREAD})

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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        # 内部头 (<internal/...>): 仅库自身编译可见, 不安装。
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_link_libraries(libcoro PRIVATE ws2_32)
elseif(LIBCORO_HAVE_OFFLOAD)
    # 线程只在开 offload 时需要 (pal_thread + offload.c)。
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(libcoro PUBLIC Threads::Threads)
endif()

if(NOT LIBCORO_HAVE_OFFLOAD)
    target_compile_definitions(libcoro PUBLIC LIBCORO_NO_OFFLOAD=1)
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

# offload 测试只在开 offload 时编入 (无线程时该测试无意义)。
set(LIBCORO_OFFLOAD_TEST "")
if(LIBCORO_HAVE_OFFLOAD)
    set(LIBCORO_OFFLOAD_TEST tests/test_offload.c)
endif()

add_executable(libcoro_tests
        tests/main.c
        ${LIBCORO_LOOP_TEST}
        tests/test_nested_task.c
        tests/test_empty_task.c
        ${LIBCORO_OFFLOAD_TEST}
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
if(LIBCORO_HAVE_OFFLOAD)
    add_test(NAME test_offload COMMAND libcoro_tests offload)
endif()
