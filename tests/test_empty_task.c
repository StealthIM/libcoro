#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "libcoro.h"

// 子task：返回一个值
static task_t* task(child_child_task) {
    gen_dec_vars(
        int value;
    );

    gen_begin(ctx);

    gen_var(value) = 32;

    gen_end(gen_var(value));
}

// 子task：返回一个值
static task_t* task(child_task) {
    gen_dec_vars(
        int value;
        task_t *task;
    );

    gen_begin(ctx);

    // 模拟一些工作
    gen_yield(async_sleep(10));

    gen_var(task) = child_child_task();
    gen_yield_from_task(gen_var(task));

    // 检查子task的future是否已完成
    printf("Check\n");
    assert(future_is_done(gen_var(task)->future));
    // 获取子task的结果
    gen_var(value) = (int) gen_var(task)->future->result;
    assert(gen_var(value) == 32);

    gen_yield(async_sleep(10));

    gen_end(gen_var(value)+10);
}

// 父task：yield from子task，并检查子task的future
static task_t* task(parent_task) {
    gen_dec_vars(
        task_t* child;
        int child_result;
        int final_result;
    );
    
    gen_begin(ctx);

    gen_yield(async_sleep(10));
    // 创建子task
    gen_var(child) = child_task();

    // yield from子task
    gen_yield_from_task(gen_var(child));

    // // 检查子task的future是否已完成
    assert(future_is_done(gen_var(child)->future));
    //
    // 获取子task的结果
    gen_var(child_result) = (int) gen_var(child)->future->result;
    printf("Val:%d\n",gen_var(child_result));
    assert(gen_var(child_result) == 42);

    printf("Child task completed with result: %d\n", gen_var(child_result));
    
    // 继续执行父task的其他逻辑
    gen_yield(async_sleep(10));
    
    gen_var(final_result) = gen_var(child_result) + 10;
    printf("End task\n");
    gen_end(gen_var(final_result));
}

static void future_check(future_t *fut, void *userdata) {
    assert(future_is_done(fut));
    assert(future_result(fut) == (void*)52);
}

int test_empty_task() {
    printf("=== Testing nested task ===\n");
    
    // 创建并运行父task
    task_t* parent = parent_task();
    future_add_done_callback(parent->future, future_check, NULL);
    
    // 运行事件循环
    loop_run(parent);

    printf("=== Test passed ===\n");
    return 0;
}