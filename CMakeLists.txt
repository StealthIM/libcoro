cmake_minimum_required(VERSION 4.0)
project(libcoro VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

option(LIBCORO_USE_LINUX_EPOLL "Use Linux epoll backend" OFF)
option(LIBCORO_USE_LINUX_SELECT "Use Linux select backend" OFF)
option(LIBCORO_USE_WIN32 "Use Win32 backend" OFF)

math(EXPR enabled_count "${LIBCORO_USE_LINUX_EPOLL}+${LIBCORO_USE_LINUX_SELECT}+${LIBCORO_USE_WIN32}")
if(enabled_count EQUAL 0)
    message(FATAL_ERROR "You must enable exactly one backend (EPOLL, SELECT, or WIN32).")
elseif(enabled_count GREATER 1)
    message(FATAL_ERROR "Multiple backends are enabled! Only one of EPOLL, SELECT, WIN32 can be ON.")
endif()

add_library(libcoro STATIC
        src/loop/linux.c
        src/loop/linux_select.c
        src/loop/windows.c
        src/future.c
        src/generator.c
        src/task.c
        include/coro/loop.h
        include/coro/future.h
        include/coro/generator.h
        include/coro/task.h
)

if(LIBCORO_USE_LINUX_EPOLL)
    target_compile_definitions(libcoro PUBLIC LIBCORO_USE_LINUX_EPOLL=1)
endif()

if(LIBCORO_USE_LINUX_SELECT)
    target_compile_definitions(libcoro PUBLIC LIBCORO_USE_LINUX_SELECT=1)
endif()

if(LIBCORO_USE_WIN32)
    target_compile_definitions(libcoro PUBLIC LIBCORO_USE_WIN32=1)
endif()

target_include_directories(libcoro
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            include/coro
)
