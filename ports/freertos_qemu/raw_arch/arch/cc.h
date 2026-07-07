#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

/*
 * lwIP arch/cc.h —— 裸机 NO_SYS=1 版 (mps2-an385 + newlib)。
 *
 * 和 FreeRTOS 版 (../arch/cc.h) 的关键差异: NO_SYS 下没有 sys_arch.h,
 * 所以 sys_prot_t / 保护宏要在这里给 (FreeRTOS 版靠 port 的 sys_arch.h)。
 * SYS_LIGHTWEIGHT_PROT=0 时保护宏是空操作 (单执行上下文, 无并发访问)。
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define LWIP_ERRNO_STDINCLUDE   1

typedef int sio_fd_t;

/* NO_SYS: 无 OS 保护原语。sys_prot_t 仍被 sys.h 声明引用, 给个占位类型。 */
typedef int sys_prot_t;

unsigned int lwip_port_rand(void);
#define LWIP_RAND() (lwip_port_rand())

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwip assert: %s\n", x); abort(); } while(0)

#endif /* LWIP_ARCH_CC_H */
