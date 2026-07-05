#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/*
 * libcoro lwIP 后端 —— 阶段 1: UNIX host port 的 lwipopts.
 *
 * 目标: 在 Linux 上用 lwIP socket 模式 (NO_SYS=0, 有 lwip_select) 跑通
 * loop 后端接缝, 全部流量走内建 loopback netif (127.0.0.1 / ::1), 不需要
 * tap 网卡、不需要 root。底层线程仍是 pthread (unix port sys_arch)。
 *
 * 这份配置刻意只开环回所需的功能。tap/外部网络、FreeRTOS、裸机 raw 模式
 * 各有独立 lwipopts (后续阶段)。
 */

/* ---- 核心模式: 带 OS (有 tcpip_thread + socket API) ---- */
#define NO_SYS                      0
#define LWIP_SOCKET                 1
#define LWIP_NETCONN                1
#define LWIP_NETIF_API              1
#define SYS_LIGHTWEIGHT_PROT        1

/* ---- 协议 ---- */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1               /* UDP 环回自唤醒需要 */
#define LWIP_DNS                    1
#define LWIP_RAW                    0
#define LWIP_ICMP                   1

/* ---- 环回 netif: 阶段 1 的唯一网络接口 ---- */
#define LWIP_HAVE_LOOPIF            1
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_LOOPBACK_MAX_PBUFS     16
#define LWIP_HAVE_SLIPIF           0

/* ---- socket 选项: 上层 pal_socket 用到 ---- */
#define SO_REUSE                    1
#define LWIP_SO_RCVTIMEO            1
#define LWIP_SO_SNDTIMEO            1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_SOCKET_SELECT          1               /* lwip_select 需要 */

/* ---- 内存: host 上放宽, 环回自连会同时开多个 pcb/pbuf ---- */
#define MEM_LIBC_MALLOC             1               /* 用 host 的 malloc, 省心 */
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (256 * 1024)
#define MEMP_NUM_TCP_PCB            32
#define MEMP_NUM_TCP_PCB_LISTEN     16
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_NETCONN            48
#define PBUF_POOL_SIZE              64

/* ---- TCP 窗口/缓冲: 环回 echo 够用即可 ---- */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)

/* ---- tcpip 线程 (socket 模式的内部线程) ---- */
#define TCPIP_THREAD_STACKSIZE      8192
#define TCPIP_MBOX_SIZE             64
#define DEFAULT_THREAD_STACKSIZE    8192
#define DEFAULT_RAW_RECVMBOX_SIZE   32
#define DEFAULT_UDP_RECVMBOX_SIZE   32
#define DEFAULT_TCP_RECVMBOX_SIZE   32
#define DEFAULT_ACCEPTMBOX_SIZE     32

/* ---- 统计/调试: 关掉, 减少噪声 ---- */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

/* ---- checksum: 环回可全部交给软件 (loopback 不校验也行, 保守起见开着) ---- */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_UDP          1

/* ---- errno: 用 host 的 <errno.h> (glibc errno 是 TLS)。
 * 注意: 千万别 #define LWIP_PROVIDE_ERRNO —— lwip/errno.h 用 #ifdef 判断,
 * 定成 0 也会触发它走 `extern int errno` (非 TLS), 和 glibc TLS errno 冲突,
 * 链接时报 "TLS definition mismatches non-TLS reference"。
 * unix port 的 arch/cc.h 已设 LWIP_ERRNO_STDINCLUDE=1 (include <errno.h>)。 ---- */

/* ---- 兼容: 用 lwip_ 前缀而非裸 socket 名, 避免和 host libc 冲突 ---- */
#define LWIP_COMPAT_SOCKETS         0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0

#endif /* LWIP_LWIPOPTS_H */
