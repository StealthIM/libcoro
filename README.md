# libcoro

libcoro 是一个用 C11 编写的协程 + 事件循环基础库,提供无栈协程(基于 Duff's device)、
future/promise、生成器和事件循环。它是 [asyncweb](https://github.com/StealthIM/asyncweb)
的底层依赖,也可作为独立库单独使用。

## 特性

- **无栈协程**:基于 Duff's device 的零额外分配协程 DSL(`gen_begin`/`gen_yield`/`gen_yield_from_task`)
- **Future/Promise**:异步结果的状态管理与完成回调
- **事件循环**:统一的异步 IO future API(recv/send/connect/accept)
- **多后端**:Linux(epoll/select)、Windows(IOCP)、lwIP(socket 模式 / 裸机 NO_SYS raw 模式)
- **可选线程池 offload**:把阻塞调用丢到 worker 线程,完成后在 loop 线程 resolve future(如异步 DNS)
- **可裁剪**:无线程目标(裸机)可关掉 offload,不引入任何线程依赖

## 安装依赖

- CMake ≥ 3.16
- C11 编译器
- 开启 offload 时(默认)需要线程库(POSIX pthread / Win32)

## 构建

必须选择一个事件循环后端(`LIBCORO_OS_BACKEND`):

```bash
cmake -B build -DLIBCORO_OS_BACKEND=epoll
cmake --build build
```

| 后端 | 平台 | 说明 |
|------|------|------|
| `epoll`  | Linux | 默认推荐 |
| `select` | Linux | 可移植回退 |
| `win`    | Windows | IOCP |
| `lwip`   | 任意 + lwIP | socket 模式(host port);裸机 raw 模式见 `ports/freertos_qemu/` |

其他选项:

- `LIBCORO_HAVE_OFFLOAD`(默认 `ON`):是否构建线程池 offload。无线程目标(裸机 `NO_SYS`)应设为 `OFF`,
  此时 `loop_run_in_thread` 返回 `NULL`,调用方需自备无线程回落(如 lwIP 原生异步 DNS)。
- `LIBCORO_BUILD_TESTS`:强制构建测试(作为顶层项目时默认开)。

### 安装 / find_package / pkg-config

```bash
cmake -B build -DLIBCORO_OS_BACKEND=epoll -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build --target install
```

下游 CMake 项目:

```cmake
find_package(libcoro REQUIRED)
target_link_libraries(your_app PRIVATE libcoro::libcoro)
```

或 pkg-config:

```bash
cc app.c $(pkg-config --cflags --libs libcoro) -o app
```

产物为静态库 `libcoro.a`(`-lcoro`),公开头安装到 `<prefix>/include/libcoro/`。

## 核心概念

- **生成器 (`gen_t`)**:一个可暂停/恢复的函数。用 DSL 宏在函数体里 `gen_yield` 出去、下次从原处恢复。
- **任务 (`task_t`)**:包装一个生成器,附带自己的完成 future(`task->future`)。用 `task(name)` / `task_arg(name)` 宏声明。
- **Future (`future_t`)**:异步结果。`gen_yield(future)` 会把 future 交给驱动,完成后恢复协程。
- **事件循环 (`loop_t`)**:线程局部的驱动器。`loop_run(task)` 跑到该任务完成为止。

`gen_yield(fut)` 用于 await 一个 **future**;`gen_yield_from_task(t)` 用于 await 一个 **子任务**。
协程内的局部变量必须用 `gen_dec_vars(...)` 声明、`gen_var(x)` 访问(栈变量在暂停期间不保留)。

## 快速示例

见 [`examples/sleep_tasks.c`](examples/sleep_tasks.c):父任务起一个子任务并 await 其结果。

```c
#include <stdio.h>
#include <libcoro/libcoro.h>

static task_t *task(worker) {
    gen_dec_vars(int result;);
    gen_begin(ctx);
    gen_yield(async_sleep(50));          /* await future */
    gen_var(result) = 21;
    gen_yield(async_sleep(50));
    gen_end((void *)(intptr_t)(gen_var(result) * 2));
}

static task_t *task(parent) {
    gen_dec_vars(task_t *child;);
    gen_begin(ctx);
    gen_var(child) = worker();
    gen_yield_from_task(gen_var(child)); /* await 子任务 */
    printf("worker -> %d\n", (int)(intptr_t)gen_var(child)->future->result);
    gen_end(NULL);
}

int main(void) {
    loop_run(parent());
    loop_destroy();
    return 0;
}
```

## API 概览

### 事件循环 (`loop.h`)

```c
loop_t  *loop_create();               /* = loop_get(): 取/建当前线程的 loop */
void     loop_destroy();
void     loop_run(task_t *task);      /* 跑到 task 完成 */
void     loop_stop();

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata);
int        loop_cancel_timer(timer_id_t id);
void       loop_call_soon(loop_cb_t cb, void *userdata);
uint64_t   loop_time_ms();
```

低层异步 IO future API(一次性 op,完成时调 `loop_io_cb_t`):

```c
int          loop_bind_handle(void *handle);   /* 每个 socket 用前调一次 */
loop_op_id_t loop_post_recv(void *handle, char *buf, unsigned long len, loop_io_cb_t cb, void *ud);
loop_op_id_t loop_post_send(void *handle, const char *buf, unsigned long len, loop_io_cb_t cb, void *ud);
loop_op_id_t loop_connect_async(void *handle, const struct sockaddr *addr, int addrlen, loop_io_cb_t cb, void *ud);
loop_op_id_t loop_accept_async(void *listen_handle, void **accept_out, loop_io_cb_t cb, void *ud);
int          loop_cancel_op(loop_op_id_t id);
```

### Future (`future.h`)

```c
future_t *future_create();
void      future_destroy(future_t *fut);
void      future_done(future_t *fut, void *result);
void      future_reject(future_t *fut, void *value);
void      future_cancel(future_t *fut);
void      future_add_done_callback(future_t *fut, future_cb_t cb, void *userdata);
void      future_remove_done_callback(future_t *fut, future_cb_t cb);
future_t *async_sleep(uint64_t ms);   /* 返回一个 ms 毫秒后完成的 future */

/* 状态查询宏 */
future_state(fut)  future_result(fut)
future_is_pending(fut)  future_is_done(fut)  future_is_rejected(fut)  future_is_cancelled(fut)
```

### 任务 (`task.h`)

```c
task_t *task_create(gen_t *gen);
void    task_run(task_t *task);       /* 挂到 loop 里跑 (不阻塞) */
void    task_destroy(task_t *task);
void    task_remove_auto_destroy(task_t *task);

/* 声明宏: task(name){...} / task_arg(name){...} (后者带 void* data 入参) */
```

### 生成器 (`generator.h`)

```c
gen_t *gen_create(gen_func *func, void *userdata);
void   gen_destroy(gen_t *gen);
void  *gen_send(gen_t *gen, void *arg);
#define gen_next(gen) gen_send((gen), NULL)
void   gen_close(gen_t *gen);

/* 协程体 DSL */
gen_dec_vars(...)  gen_begin(ctx)  gen_var(x)  gen_userdata()
gen_yield(future)  gen_yield_from_task(task)  gen_end(val)  gen_return(val)  gen_cleanup()
```

### Offload 线程池 (`offload.h`)

```c
/* 把 fn(arg) 丢到 worker 线程, 返回一个在完成时 resolve 的 future。
 * 无 offload 能力的构建 (LIBCORO_HAVE_OFFLOAD=OFF) 返回 NULL。 */
future_t *loop_run_in_thread(void *(*fn)(void *arg), void *arg);
```

## 目录

- `include/libcoro/` — 公开头(安装)
- `include/internal/` — 内部头(不安装)
- `src/` — 实现(loop 后端在 `src/loop/`)
- `ports/` — lwIP host port + FreeRTOS/裸机 QEMU port(含 raw NO_SYS 后端)
- `examples/` — 最小示例
- `tests/` — 测试

## 许可证

MIT
