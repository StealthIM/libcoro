#pragma once

/*
 * libcoro —— 协程 + 事件循环基础库 (公开 API 汇总头)。
 *
 * 用户只需 #include <libcoro/libcoro.h>。各子头也可单独 include。
 *   - generator.h : Duff's-device 无栈协程 DSL (gen_begin/gen_yield/...)
 *   - future.h    : 异步结果 (future/promise)
 *   - task.h      : 协程任务 (task_run + task(name)/task_arg(name) 宏)
 *   - loop.h      : 事件循环 + 低层异步 IO (recv/send/connect/accept)
 *   - offload.h   : 阻塞调用 offload 到线程池 (loop_run_in_thread)
 */

#include <libcoro/generator.h>
#include <libcoro/future.h>
#include <libcoro/task.h>
#include <libcoro/loop.h>
#include <libcoro/offload.h>
