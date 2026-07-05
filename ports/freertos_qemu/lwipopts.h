#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/*
 * libcoro lwIP 后端 —— 阶段 2a: FreeRTOS + QEMU mps2-an385.
 *
 * 和阶段 1 host 版 (ports/lwip/lwipopts.h) 的关键差异:
 *   - sys_arch 走 FreeRTOS (contrib/ports/freertos), 非 pthread unix port。
 *   - 内存用 lwIP 自己的静态池 (MEM_LIBC_MALLOC=0): 裸机 newlib malloc 受
 *     _sbrk 限制且非线程安全, 不能给 lwIP 用。lwIP 池是静态数组, 编译期定大小。
 *   - TCPIP core locking: FreeRTOS 多任务下用 core-lock 模型更清晰。
 *   - 仍只挂 loopback netif (127/8 栈内), 不碰 QEMU 网卡模型。
 */

/* ---- 核心模式: 带 OS ---- */
#define NO_SYS                      0
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define LWIP_NETIF_API              1
#define SYS_LIGHTWEIGHT_PROT        1

/* ---- FreeRTOS sys_arch 集成 ---- */
#define LWIP_TCPIP_CORE_LOCKING     1
#define LWIP_COMPAT_MUTEX           0       /* 用真 mutex (sys_arch 有 struct 包装) */
#define LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS 1
#define TCPIP_THREAD_STACKSIZE      2048    /* 字数 (上面开了 IS_STACKWORDS) */
#define TCPIP_THREAD_PRIO           ( 4 )
#define DEFAULT_THREAD_STACKSIZE    1024
#define TCPIP_MBOX_SIZE             16
#define DEFAULT_RAW_RECVMBOX_SIZE   12
#define DEFAULT_UDP_RECVMBOX_SIZE   12
#define DEFAULT_TCP_RECVMBOX_SIZE   12
#define DEFAULT_ACCEPTMBOX_SIZE     12

/* ---- 协议 ---- */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1               /* UDP 环回自唤醒需要 */
#define LWIP_DNS                    1
#define LWIP_RAW                    0
#define LWIP_ICMP                   1

/* ---- 环回 netif ---- */
#define LWIP_HAVE_LOOPIF            1
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_LOOPBACK_MAX_PBUFS     16

/* ---- socket 选项 ---- */
#define SO_REUSE                    1
#define LWIP_SO_RCVTIMEO            1
#define LWIP_SO_SNDTIMEO            1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_SOCKET_SELECT          1
#define LWIP_NETCONN_SEM_PER_THREAD 0

/* ---- 内存: lwIP 自己的静态池 (不用 newlib malloc) ---- */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (64 * 1024)
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_TCP_PCB_LISTEN     8
#define MEMP_NUM_UDP_PCB            6
#define MEMP_NUM_NETCONN            24
#define MEMP_NUM_TCP_SEG            32
#define PBUF_POOL_SIZE              48

/* ---- TCP 窗口/缓冲 ---- */
#define TCP_MSS                     1460
#define TCP_WND                     (6 * TCP_MSS)
#define TCP_SND_BUF                 (6 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)

/* ---- 统计/调试 ---- */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

/* ---- checksum ---- */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_UDP          1

/* ---- errno: 用 newlib 的 <errno.h> (同阶段 1: 别 #define LWIP_PROVIDE_ERRNO) ---- */
#define LWIP_ERRNO_STDINCLUDE       1

/* ---- 兼容 ---- */
#define LWIP_COMPAT_SOCKETS         0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0

#endif /* LWIP_LWIPOPTS_H */
