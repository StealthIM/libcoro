#ifndef LIBCORO_RAW_INET_H
#define LIBCORO_RAW_INET_H
/*
 * 裸机 NO_SYS 版最小 sockaddr 定义 (阶段 2b)。
 *
 * lwIP 的 struct sockaddr / sockaddr_in / AF_INET 全被 LWIP_SOCKET gate 掉
 * (见 lwip/sockets.h 的 #if LWIP_SOCKET)。raw 构建 LWIP_SOCKET=0, 拿不到这些,
 * 但 loop 的 IO API 签名 (loop.h) 用 struct sockaddr, 测试也要 sockaddr_in。
 * 这里给一份最小 BSD 风格定义, 只够 IPv4 环回用。字节序/端口宏复用 lwIP 的
 * PP_HTONS/PP_HTONL/lwip_ntohs (在 lwip/def.h, 不受 LWIP_SOCKET 影响)。
 */

#include <stdint.h>
#include "lwip/def.h"

typedef uint8_t  sa_family_t;
typedef uint16_t in_port_t;

#ifndef AF_INET
#define AF_INET   2
#endif
#ifndef AF_INET6
#define AF_INET6  10
#endif

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK  0x7f000001UL
#endif

struct in_addr { uint32_t s_addr; };

struct sockaddr {
    uint8_t     sa_len;
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    uint8_t        sin_len;
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

struct sockaddr_storage {
    uint8_t        s2_len;
    sa_family_t    ss_family;
    char           s2_data[26];
};

#endif /* LIBCORO_RAW_INET_H */
