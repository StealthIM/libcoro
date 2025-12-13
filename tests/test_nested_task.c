#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "libcoro.h"

// 子task：返回一个值
task_t* task(child_task) {
    gen_dec_vars(
        int value;
    );
    
    gen_begin(ctx);
    
    // 模拟一些工作
    gen_yield(async_sleep(10));
    
    gen_var(value) = 42;

    gen_yield(async_sleep(10));

    gen_end(gen_var(value));
}

// 父task：yield from子task，并检查子task的future
task_t* task(parent_task) {
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
    assert(gen_var(child_result) == 42);

    printf("Child task completed with result: %d\n", gen_var(child_result));
    
    // 继续执行父task的其他逻辑
    gen_yield(async_sleep(10));
    
    gen_var(final_result) = gen_var(child_result) + 10;
    gen_end(gen_var(final_result));
}

int test_nested_task() {
    printf("=== Testing nested task ===\n");
    
    // 创建并运行父task
    task_t* parent = parent_task();
    
    // 运行事件循环
    loop_run(parent);

    printf("=== Test passed ===\n");
    return 0;
}