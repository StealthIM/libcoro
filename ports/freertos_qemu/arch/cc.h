#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

/*
 * lwIP arch/cc.h —— 裸机 (mps2-an385 + newlib) 版。
 *
 * FreeRTOS port 只提供 sys_arch.h, cc.h 要自己写。相比 unix 版, 裸机这份:
 *   - 用 newlib 的 <errno.h> (TLS 由 FreeRTOS-aware 的 reent 处理, 见下)。
 *   - LWIP_RAND 用一个简单的 LCG (loopback 测试不需要密码学随机)。
 *   - timeval 用 newlib 的。
 */

#include <errno.h>

/* newlib 提供 <errno.h> 全套错误码; 别 #define LWIP_PROVIDE_ERRNO。 */
#define LWIP_ERRNO_STDINCLUDE   1

/* sio (串口 slip) 用不到, 给个占位类型满足 lwip/sio.h 前置声明。 */
typedef int sio_fd_t;

/* 注意: sys_prot_t 由 FreeRTOS port 的 sys_arch.h 定义 (SYS_LIGHTWEIGHT_PROT),
 * 这里别重复定义, 否则冲突。 */

/* 简单 LCG 随机 (仅用于 lwIP 端口/序列号扰动, 环回测试足够)。 */
unsigned int lwip_port_rand(void);
#define LWIP_RAND() (lwip_port_rand())

/* 平台断言: 落到 FreeRTOS/newlib 的方式在别处; 这里给个最简实现声明。 */
#include <stdio.h>
#include <stdlib.h>
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwip assert: %s\n", x); abort(); } while(0)

#endif /* LWIP_ARCH_CC_H */
