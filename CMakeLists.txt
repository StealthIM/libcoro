cmake_minimum_required(VERSION 4.0)
project(libcoro VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

set(LIBCORO_OS_BACKEND "" CACHE STRING "Choose OS backend: epoll, select, win")
set_property(CACHE LIBCORO_OS_BACKEND PROPERTY STRINGS epoll select win)

add_library(libcoro)

list(APPEND SRC_FILES
        src/future.c
        src/generator.c
        src/task.c
        include/coro/loop.h
        include/coro/future.h
        include/coro/generator.h
        include/coro/task.h
)

if(LIBCORO_OS_BACKEND STREQUAL "epoll")
    list(APPEND SRC_FILES src/loop/linux_epoll.c)

elseif(LIBCORO_OS_BACKEND STREQUAL "select")
    list(APPEND SRC_FILES src/loop/linux_select.c)

elseif(LIBCORO_OS_BACKEND STREQUAL "win")
    list(APPEND SRC_FILES src/loop/windows.c)
else()
    message(FATAL_ERROR "No valid OS backend selected. Please choose one of: epoll, select, win")
endif()

target_sources(libcoro PRIVATE ${SRC_FILES})

target_include_directories(libcoro
        PUBLIC
            include/libcoro.h
        PRIVATE
            include/coro
)

add_executable(libcoro_tests
        test/main.c
        tests/test_loop.c
)
target_link_libraries(libcoro_tests PRIVATE libcoro)

enable_testing()
add_test(NAME test_loop COMMAND libcoro_tests loop)
