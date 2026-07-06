#!/usr/bin/env bash
# 阶段 2a + TLS: FreeRTOS + lwIP + libcoro + wolfSSL + asyncweb 完整栈。
# 用法: build_tls.sh <out.elf>
#
# 分五组编译 (同 build.sh 的 task.h 同名冲突处理, 加两组):
#   A. FreeRTOS 内核 + startup + boot_fr + pal_thread (FreeRTOS 头)
#   B. lwIP 栈 + FreeRTOS sys_arch (lwIP + FreeRTOS 头)
#   C. libcoro 核心 + loop 后端 (libcoro 私有头 + lwIP 头)
#   D. wolfSSL (ECC-only, -DWOLFSSL_USER_SETTINGS)
#   E. asyncweb TLS + future_socket + common + 测试 (libcoro私有头+lwIP+wolfSSL头)
set -e

OUT="${1:-/tmp/fr_tls.elf}"
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT

LIBCORO="$(cd "$(dirname "$0")/../.." && pwd)"
QD="$LIBCORO/ports/freertos_qemu"
FR="$HOME/.cache/FreeRTOS-Kernel"
LWIP="$HOME/.cache/lwip"
LWIP_FR="$LWIP/contrib/ports/freertos"
WOLF="$HOME/.cache/wolfssl-5.9.2"
AW="$(cd "$LIBCORO/../asyncweb" && pwd)"

CC=arm-none-eabi-gcc
CFLAGS="-mcpu=cortex-m3 -mthumb -g -O2 -ffreestanding -Wall -DLIBCORO_LWIP=1"

INC_FR="-I$QD -I$FR/include -I$FR/portable/GCC/ARM_CM3 -I$LIBCORO/include/private"
INC_LWIP="-I$QD -I$LWIP/src/include -I$LWIP_FR/include -I$FR/include -I$FR/portable/GCC/ARM_CM3"
INC_CORO="-I$QD -I$LIBCORO/include/private -I$LIBCORO/include/public -I$LWIP/src/include -I$LWIP_FR/include"
INC_WOLF="-I$QD -I$WOLF -DWOLFSSL_USER_SETTINGS"
# asyncweb TLS: 要 libcoro 私有头 + asyncweb 私有头 + lwIP 头 + wolfSSL 头
INC_AW="-I$QD -I$AW/include/private -I$LIBCORO/include/private -I$LIBCORO/include/public \
        -I$LWIP/src/include -I$LWIP_FR/include -I$WOLF -DWOLFSSL_USER_SETTINGS"

compile() {
  local inc="$1"; shift
  for f in "$@"; do
    local o="$OBJ/$(echo "$f" | md5sum | cut -c1-12).o"
    $CC $CFLAGS $inc -c "$f" -o "$o"
    echo "$o"
  done
}

OBJS=""

# 组 A
OBJS+=" $(compile "$INC_FR" \
  "$QD/startup_mps2.c" "$QD/boot_fr.c" \
  "$LIBCORO/src/pal/thread_freertos.c" \
  "$FR/tasks.c" "$FR/list.c" "$FR/queue.c" "$FR/timers.c" \
  "$FR/portable/GCC/ARM_CM3/port.c" "$FR/portable/MemMang/heap_4.c")"

# 组 B
OBJS+=" $(compile "$INC_LWIP" \
  "$LWIP/src/core/init.c" "$LWIP/src/core/def.c" "$LWIP/src/core/dns.c" \
  "$LWIP/src/core/inet_chksum.c" "$LWIP/src/core/ip.c" "$LWIP/src/core/mem.c" \
  "$LWIP/src/core/memp.c" "$LWIP/src/core/netif.c" "$LWIP/src/core/pbuf.c" \
  "$LWIP/src/core/raw.c" "$LWIP/src/core/stats.c" "$LWIP/src/core/sys.c" \
  "$LWIP/src/core/tcp.c" "$LWIP/src/core/tcp_in.c" "$LWIP/src/core/tcp_out.c" \
  "$LWIP/src/core/timeouts.c" "$LWIP/src/core/udp.c" \
  "$LWIP/src/core/ipv4/acd.c" "$LWIP/src/core/ipv4/etharp.c" \
  "$LWIP/src/core/ipv4/icmp.c" "$LWIP/src/core/ipv4/ip4_addr.c" \
  "$LWIP/src/core/ipv4/ip4.c" "$LWIP/src/core/ipv4/ip4_frag.c" \
  "$LWIP/src/core/ipv6/ethip6.c" "$LWIP/src/core/ipv6/icmp6.c" \
  "$LWIP/src/core/ipv6/inet6.c" "$LWIP/src/core/ipv6/ip6_addr.c" \
  "$LWIP/src/core/ipv6/ip6.c" "$LWIP/src/core/ipv6/ip6_frag.c" \
  "$LWIP/src/core/ipv6/mld6.c" "$LWIP/src/core/ipv6/nd6.c" \
  "$LWIP/src/api/api_lib.c" "$LWIP/src/api/api_msg.c" "$LWIP/src/api/err.c" \
  "$LWIP/src/api/netbuf.c" "$LWIP/src/api/netdb.c" "$LWIP/src/api/netifapi.c" \
  "$LWIP/src/api/sockets.c" "$LWIP/src/api/tcpip.c" \
  "$LWIP/src/netif/ethernet.c" "$LWIP_FR/sys_arch.c")"

# 组 C
OBJS+=" $(compile "$INC_CORO" \
  "$LIBCORO/src/loop/lwip_select.c" "$LIBCORO/src/offload.c" \
  "$LIBCORO/src/future.c" "$LIBCORO/src/task.c" "$LIBCORO/src/generator.c")"

# 组 D: wolfSSL (ECC + AES-GCM + SHA256 + TLS1.3)
OBJS+=" $(compile "$INC_WOLF" \
  "$WOLF/src/ssl.c" "$WOLF/src/internal.c" "$WOLF/src/tls.c" \
  "$WOLF/src/tls13.c" "$WOLF/src/keys.c" "$WOLF/src/wolfio.c" \
  "$WOLF/wolfcrypt/src/asn.c" "$WOLF/wolfcrypt/src/coding.c" \
  "$WOLF/wolfcrypt/src/ecc.c" "$WOLF/wolfcrypt/src/aes.c" \
  "$WOLF/wolfcrypt/src/sha256.c" "$WOLF/wolfcrypt/src/sha.c" \
  "$WOLF/wolfcrypt/src/random.c" "$WOLF/wolfcrypt/src/hmac.c" \
  "$WOLF/wolfcrypt/src/hash.c" "$WOLF/wolfcrypt/src/wc_encrypt.c" \
  "$WOLF/wolfcrypt/src/wc_port.c" "$WOLF/wolfcrypt/src/memory.c" \
  "$WOLF/wolfcrypt/src/error.c" "$WOLF/wolfcrypt/src/logging.c" \
  "$WOLF/wolfcrypt/src/wolfmath.c" "$WOLF/wolfcrypt/src/sp_int.c" \
  "$WOLF/wolfcrypt/src/sp_c32.c" "$WOLF/wolfcrypt/src/kdf.c")"

# 组 E: asyncweb TLS + future_socket + pal_socket + common + 测试
OBJS+=" $(compile "$INC_AW" \
  "$AW/src/tls/wolfssl.c" "$AW/src/sock/future_socket.c" \
  "$AW/src/sock/pal_socket/lwip.c" "$AW/src/common.c" \
  "$QD/test_fr_tls.c")"

$CC $CFLAGS --specs=rdimon.specs -T "$QD/mps2_an385.ld" -nostartfiles \
  $OBJS -o "$OUT" 2>&1 | grep -ivE "RWX permissions|LOAD segment" || true

echo "built: $OUT"
