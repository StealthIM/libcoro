#ifndef LWIP_LWIPOPTS_RAW_H
#define LWIP_LWIPOPTS_RAW_H

/*
 * libcoro lwIP 后端 —— 裸机 (NO_SYS=1, raw/callback API)。
 *
 * 和 FreeRTOS socket 模式 (lwipopts.h) 的关键差异:
 *   - NO_SYS=1: 没有 OS 抽象层, 没有 tcpip_thread, 没有 sys_arch mbox/sem。
 *     全部在单一执行上下文里跑, 由 loop_run 手动 poll 驱动
 *     (sys_check_timeouts + netif_poll_all)。
 *   - LWIP_SOCKET=0 / LWIP_NETCONN=0: 没有 BSD socket / netconn API,
 *     只有 raw TCP 回调 API (tcp_new/bind/listen/connect + tcp_recv/sent/...)。
 *     libcoro 的 lwip_raw.c loop 后端直接用它。
 *   - 没有真线程 → offload 线程池编译关闭, DNS 走 dns_gethostbyname 原生异步。
 *   - 环回 netif 在单线程模式下: 排队的 loopback pbuf 由 netif_poll_all()
 *     在 loop 里手动排空 (LWIP_NETIF_LOOPBACK_MULTITHREADING=0)。
 *
 * 仍只挂 loopback netif (127/8 栈内), 不碰 QEMU 网卡模型。
 */

/* ---- 核心模式: 无 OS ---- */
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_NETIF_API              0
#define SYS_LIGHTWEIGHT_PROT        0

/* ---- 协议 ---- */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_RAW                    0
#define LWIP_ICMP                   1

/* ---- 回调风格 API (raw) ---- */
#define LWIP_CALLBACK_API           1

/* ---- 环回 netif (单线程: 手动 poll 排空) ---- */
#define LWIP_HAVE_LOOPIF                    1
#define LWIP_NETIF_LOOPBACK                 1
#define LWIP_LOOPBACK_MAX_PBUFS             16
#define LWIP_NETIF_LOOPBACK_MULTITHREADING  0

/* ---- 内存: lwIP 自己的静态池 (不用 newlib malloc) ---- */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (64 * 1024)
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_TCP_PCB_LISTEN     8
#define MEMP_NUM_UDP_PCB            6
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

/* ---- errno: 用 newlib 的 <errno.h> ---- */
#define LWIP_ERRNO_STDINCLUDE       1

#endif /* LWIP_LWIPOPTS_RAW_H */
