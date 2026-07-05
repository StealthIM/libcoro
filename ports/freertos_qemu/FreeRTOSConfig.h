#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*
 * QEMU mps2-an385 (Cortex-M3) 的 FreeRTOS 配置。
 *
 * 关键: 把 FreeRTOS ARM_CM3 port 的三个 handler 映射到 CMSIS 标准名,
 * 这样 startup_mps2.c 的向量表用标准名 (SVC/PendSV/SysTick_Handler) 即可。
 */

#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCPU_CLOCK_HZ                      ( 25000000UL )  /* mps2 默认 25MHz */
#define configTICK_RATE_HZ                      ( 1000 )
#define configMAX_PRIORITIES                    ( 7 )
#define configMINIMAL_STACK_SIZE                ( 256 )
#define configTOTAL_HEAP_SIZE                   ( 128 * 1024 )  /* lwIP+任务栈 */
#define configMAX_TASK_NAME_LEN                 ( 16 )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                16
#define configTIMER_TASK_STACK_DEPTH            ( configMINIMAL_STACK_SIZE * 2 )

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 1   /* lwIP NETCONN_SEM_PER_THREAD 用 */

/* 钩子 / 检查 */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1

/* Cortex-M3: 无 __NVIC_PRIO_BITS 时用 3 (mps2 是 3 位优先级) */
#define configPRIO_BITS                         3
#define configKERNEL_INTERRUPT_PRIORITY         ( 7 << (8 - configPRIO_BITS) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 5 << (8 - configPRIO_BITS) )

/* API 裁剪 */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xSemaphoreGetMutexHolder        1

/* assert: 失败时停住 (QEMU 里可用调试器看) */
#define configASSERT( x )   if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ); }

/* 关键: 中断 handler 映射到 CMSIS 标准名 (见向量表) */
#define vPortSVCHandler        SVC_Handler
#define xPortPendSVHandler     PendSV_Handler
#define xPortSysTickHandler    SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
