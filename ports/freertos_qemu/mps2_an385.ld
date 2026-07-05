/*
 * QEMU mps2-an385 (Cortex-M3) linker script.
 *
 * 内存布局 (QEMU hw/arm/mps2.c 的 AN385):
 *   0x00000000  4MB  ZBT SRAM, 当 code/flash 用 (含向量表)
 *   0x20000000  4MB  ZBT SRAM, 当 data/bss/heap/stack 用
 *
 * 向量表须落在 0x0: Cortex-M 复位时从 [0x0] 取初始 SP, 从 [0x4] 取复位入口。
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

/* 栈从 RAM 顶向下增长; newlib heap 在 bss 之后向上, _sbrk 用 end.._heap_limit。 */
_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Stack_Size = 0x8000;    /* 32K 给 main/ISR 栈 (heap 顶留在此之下) */
_heap_limit = _estack - _Min_Stack_Size;

SECTIONS
{
    .isr_vector : {
        KEEP(*(.isr_vector))
    } > FLASH

    .text : {
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* .data 的加载地址在 FLASH, 运行地址在 RAM; startup 负责搬运。 */
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /* heap 起点 end 落在 bss 之后; _sbrk 从这里长到 _heap_limit。
     * 额外预留栈, 链接期检查 RAM 够不够 (heap 至少要有点空间)。 */
    ._user_heap_stack : {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > RAM

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
