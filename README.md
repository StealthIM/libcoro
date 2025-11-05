# libcoro

libcoro是一个用C语言编写的协程库，提供了异步编程的基本组件，包括任务(task)、期约(future)、生成器(generator)和事件循环(loop)。

## 特性

- **跨平台支持**: 支持Linux(epoll/select)和Windows(I/O完成端口)多种事件循环后端
- **轻量级协程**: 基于生成器实现的协程机制
- **Future/Promise模式**: 异步操作的结果管理
- **事件驱动**: 高效的事件循环机制
- **定时器支持**: 支持基于时间的异步操作

## 组件介绍

### 任务 (Task)
任务是协程的基本执行单元，封装了生成器和期约。

### 期约 (Future)
用于表示异步操作的结果，支持回调注册和状态管理。

### 生成器 (Generator)
实现了类似Python生成器的功能，支持yield和yield from语义。

### 事件循环 (Loop)
提供事件驱动的运行环境，支持文件描述符事件、定时器和即时回调。

## API文档

### 事件循环 (Loop) API

#### loop_create()
```c
loop_t *loop_create();
```
获取当前线程的事件循环实例，如果不存在则创建一个新的。

#### loop_destroy()
```c
void loop_destroy();
```
销毁当前线程的事件循环实例，释放所有相关资源。

#### loop_run()
```c
void loop_run(task_t *task);
```
运行事件循环，并执行指定的任务。当任务完成时循环会自动停止。

#### loop_stop()
```c
void loop_stop();
```
停止事件循环。

#### loop_register_handle()
```c
int loop_register_handle(void *handle, loop_cb_t cb, void *userdata);
```
注册一个文件描述符（或socket句柄）到事件循环中，当有数据可读时会调用回调函数。

参数：
- handle: 文件描述符或socket句柄
- cb: 回调函数
- userdata: 用户数据

#### loop_unregister_handle()
```c
int loop_unregister_handle(void *handle, bool close_socket);
```
从事件循环中注销一个文件描述符（或socket句柄）。

参数：
- handle: 文件描述符或socket句柄
- close_socket: 是否同时关闭socket

#### loop_add_timer()
```c
timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata);
```
添加一个定时器，在指定的毫秒数后调用回调函数。

参数：
- when_ms: 延迟的毫秒数
- cb: 回调函数
- userdata: 用户数据

返回值：定时器ID，用于取消定时器

#### loop_cancel_timer()
```c
int loop_cancel_timer(timer_id_t id);
```
取消指定ID的定时器。

参数：
- id: 定时器ID

#### loop_call_soon()
```c
void loop_call_soon(loop_cb_t cb, void *userdata);
```
将回调函数添加到事件循环的下一次迭代中执行。

参数：
- cb: 回调函数
- userdata: 用户数据

#### loop_time_ms()
```c
uint64_t loop_time_ms();
```
获取当前时间戳（毫秒）。

### 期约 (Future) API

#### future_create()
```c
future_t *future_create();
```
创建一个新的期约对象。

#### future_destroy()
```c
void future_destroy(future_t *fut);
```
销毁期约对象，释放相关资源。

#### future_done()
```c
void future_done(future_t *fut, void *result);
```
将期约标记为已完成，并设置结果。

参数：
- fut: 期约对象
- result: 结果数据

#### future_reject()
```c
void future_reject(future_t *fut, void *value);
```
将期约标记为已拒绝，并设置错误值。

参数：
- fut: 期约对象
- value: 错误值

#### future_cancel()
```c
void future_cancel(future_t *fut);
```
将期约标记为已取消。

参数：
- fut: 期约对象

#### future_add_done_callback()
```c
void future_add_done_callback(future_t *fut, future_cb_t cb, void *userdata);
```
为期约添加完成回调函数。

参数：
- fut: 期约对象
- cb: 回调函数
- userdata: 用户数据

#### async_sleep()
```c
future_t *async_sleep(uint64_t ms);
```
创建一个在指定毫秒数后完成的期约。

参数：
- ms: 延迟的毫秒数

### 任务 (Task) API

#### task_create()
```c
task_t *task_create(gen_t *gen);
```
创建一个新任务，封装指定的生成器。

参数：
- gen: 生成器对象

#### task_destroy()
```c
void task_destroy(task_t *task);
```
销毁任务对象，释放相关资源。

参数：
- task: 任务对象

#### task_run()
```c
void task_run(task_t *task);
```
在事件循环中运行任务。

参数：
- task: 任务对象

### 生成器 (Generator) API

#### gen_create()
```c
gen_t *gen_create(gen_func *func, void *userdata);
```
创建一个新的生成器对象。

参数：
- func: 生成器函数
- userdata: 用户数据

#### gen_destroy()
```c
void gen_destroy(gen_t *gen);
```
销毁生成器对象，释放相关资源。

参数：
- gen: 生成器对象

#### gen_send()
```c
void *gen_send(gen_t *gen, void *arg);
```
向生成器发送一个值并恢复执行。

参数：
- gen: 生成器对象
- arg: 发送的值

#### gen_next()
```c
void *gen_next(gen);
```
获取生成器的下一个值。

参数：
- gen: 生成器对象

#### gen_close()
```c
void gen_close(gen_t *gen);
```
关闭生成器，执行清理操作。

参数：
- gen: 生成器对象

## 构建选项

在构建时可以选择不同的后端实现：

- `LIBCORO_USE_LINUX_EPOLL`: 使用Linux epoll后端
- `LIBCORO_USE_LINUX_SELECT`: 使用Linux select后端
- `LIBCORO_USE_WIN32`: 使用Win32后端

注意：必须且只能启用一个后端。

## 安装依赖

需要CMake 4.0或更高版本。

## 构建方法

```bash
mkdir build
cd build
# 选择一个后端启用
cmake .. -DLIBCORO_USE_LINUX_EPOLL=ON
make
```

## 使用示例

以下是一个简单的示例，展示如何使用libcoro创建和运行一个协程任务：

```c
#include "libcoro.h"
#include <stdio.h>

// 协程函数示例
gen_ret_t example_coro_func(gen_ctx_t *ctx, void *arg) {
    (void)arg;
    
    // 开始协程
    gen_begin(ctx);
    
    // 第一次yield
    gen_yield("First yield");

    // 等待1秒
    {
        future_t *sleep_fut = async_sleep(1000);
        gen_yield_from(sleep_fut);
    }

    // 第二次yield
    gen_yield("Second yield");

    // 再等待1秒
    {
        future_t *sleep_fut = async_sleep(1000);
        gen_yield_from(sleep_fut);
    }

    // 完成
    gen_end("Coroutine finished");
}

int main() {
    // 创建事件循环
    loop_t *loop = loop_create();
    
    // 创建生成器
    gen_t *gen = gen_create(example_coro_func, NULL);
    
    // 创建任务
    task_t *task = task_create(gen);
    
    // 运行事件循环
    loop_run(task);
    
    // 清理资源
    task_destroy(task);
    loop_destroy();
    
    return 0;
}
```

## 许可证

MIT许可证