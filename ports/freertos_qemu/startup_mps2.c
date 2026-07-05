/*
 * QEMU mps2-an385 (Cortex-M3) 最小 startup。
 *
 * 职责:
 *   - 在 0x0 放向量表 (初始 SP + 复位入口 + 关键异常/中断)。
 *   - Reset_Handler: 搬 .data, 清 .bss, 调 main。
 *   - FreeRTOS 的三个 handler 通过 FreeRTOSConfig.h 映射到 CMSIS 名
 *     (SVC_Handler/PendSV_Handler/SysTick_Handler), 这里直接引用标准名。
 *
 * semihosting I/O 由 newlib rdimon (--specs=rdimon.specs) 提供,
 * 需在 main 前调 initialise_monitor_handles() (rdimon 的入口)。
 */

#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

/* newlib rdimon: 初始化 semihosting 文件句柄 (stdout 等)。 */
extern void initialise_monitor_handles(void);

void Reset_Handler(void);
void Default_Handler(void);

/* FreeRTOS ARM_CM3 port 提供 (经 FreeRTOSConfig.h 映射为 CMSIS 名)。 */
void SVC_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));

/* CMSDK UART0 中断等其余外设中断: 默认落到 Default_Handler。 */
void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));

/* 向量表: 前 16 个是 Cortex-M 架构定义, 之后是外设中断 (这里够用即可)。 */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))(&_estack),  /* 0: 初始 SP */
    Reset_Handler,               /* 1: 复位 */
    NMI_Handler,                 /* 2 */
    HardFault_Handler,           /* 3 */
    MemManage_Handler,           /* 4 */
    BusFault_Handler,            /* 5 */
    UsageFault_Handler,          /* 6 */
    0, 0, 0, 0,                  /* 7-10 保留 */
    SVC_Handler,                 /* 11: SVCall (FreeRTOS) */
    0, 0,                        /* 12-13 保留 */
    PendSV_Handler,              /* 14: PendSV (FreeRTOS 上下文切换) */
    SysTick_Handler,             /* 15: SysTick (FreeRTOS tick) */
    /* 16+: 外设中断, 全部默认 handler (mps2 有 UART/timer 等, 本测试用不到) */
};

void Reset_Handler(void) {
    /* 搬运 .data (LMA in FLASH -> VMA in RAM) */
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    /* 清零 .bss */
    for (uint32_t *bss = &_sbss; bss < &_ebss; ) {
        *bss++ = 0;
    }

    initialise_monitor_handles();
    main();

    /* main 返回后停住 */
    for (;;) {}
}

void Default_Handler(void) {
    for (;;) {}
}

/* newlib 的 __libc_init_array / __libc_fini_array 需要这两个符号;
 * 裸机无 init/fini 段, 给空 thumb stub 即可 (否则 exit() 链接报
 * "undefined reference to _fini" + relocation 错误)。 */
void _init(void) {}
void _fini(void) {}

/* newlib malloc 依赖 _sbrk 扩展堆。rdimon 的默认 _sbrk 走 semihosting
 * SYS_HEAPINFO, 在 QEMU 上返回无效范围导致 malloc 返回 NULL。这里自己实现:
 * 堆从 linker 的 end 长到 _heap_limit (栈预留之下)。
 *
 * 注意: 这是 newlib 堆 (pal_thread 的 malloc/free 用它)。FreeRTOS 自己的
 * 堆是 heap_4 的 configTOTAL_HEAP_SIZE, 两者独立。offload worker 在 task
 * 里跑, 会同时用到两者。newlib 的 malloc 非线程安全, 但 offload 各 job 的
 * 分配都在 pal_thread_create/join 的主 task 侧, worker 内不 malloc, 够用。 */
extern uint32_t end;          /* linker: 堆底 (bss 之后) */
extern uint32_t _heap_limit;  /* linker: 堆顶 (栈预留之下) */

void *_sbrk(int incr) {
    static uint8_t *heap = 0;
    if (heap == 0) heap = (uint8_t *)&end;

    uint8_t *prev = heap;
    if (heap + incr > (uint8_t *)&_heap_limit) {
        return (void *)-1;    /* 堆耗尽 */
    }
    heap += incr;
    return (void *)prev;
}
