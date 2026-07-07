#!/usr/bin/env bash
# TLS: 裸机 raw + wolfSSL + 高层 HTTPS server 到 QEMU mps2-an385。
# 用法: build_https.sh <out.elf>
#
# 在 build_tls_raw.sh (裸机 raw + wolfSSL) 基础上, 组 E 再加 http_server.c +
# stream.c (高层 HTTPS server 全链, 内存证书经 use_tls_mem)。
set -e

OUT="${1:-/tmp/fr_https.elf}"
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT

LIBCORO="$(cd "$(dirname "$0")/../.." && pwd)"
QD="$LIBCORO/ports/freertos_qemu"
LWIP="$HOME/.cache/lwip"
WOLF="$HOME/.cache/wolfssl-5.9.2"
AW="$(cd "$LIBCORO/../asyncweb" && pwd)"

CC=arm-none-eabi-gcc
CFLAGS="-mcpu=cortex-m3 -mthumb -g -O1 -ffreestanding -Wall -DLIBCORO_LWIP_RAW=1 ${LOOP_DEBUG:+-DLOOP_DEBUG}"

INC_LWIP="-I$QD/raw_arch -I$LWIP/src/include"
INC_CORO="-I$QD/raw_arch -I$LIBCORO/include -I$LWIP/src/include -I$QD"
INC_WOLF="-I$QD -I$WOLF -DWOLFSSL_USER_SETTINGS"
# asyncweb TLS: asyncweb 私有 + libcoro 私有 + raw_arch + lwIP + wolfSSL
INC_AW="-I$QD/raw_arch -I$AW/include -I$LIBCORO/include -I$LWIP/src/include -I$WOLF -I$QD -DWOLFSSL_USER_SETTINGS"

compile() {
  local inc="$1"; shift
  for f in "$@"; do
    local o="$OBJ/$(echo "$f" | md5sum | cut -c1-12).o"
    $CC $CFLAGS $inc -c "$f" -o "$o"
    echo "$o"
  done
}

OBJS=""

# 组 A: startup + 裸机 boot
OBJS+=" $(compile "-I$QD/raw_arch -I$QD" \
  "$QD/startup_mps2.c" "$QD/boot_raw.c")"

# 组 B: lwIP core (NO_SYS)
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
  "$LWIP/src/netif/ethernet.c")"

# 组 C: libcoro 核心 + lwip_raw loop 后端
OBJS+=" $(compile "$INC_CORO" \
  "$LIBCORO/src/loop/lwip_raw.c" \
  "$LIBCORO/src/future.c" "$LIBCORO/src/task.c" "$LIBCORO/src/generator.c")"

# 组 D: wolfSSL (ECC + AES-GCM + SHA256 + TLS1.3) —— 同 build_tls.sh
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

# 组 E: asyncweb 高层 HTTPS server 全链 (真 wolfssl.c) + http_server + stream +
#        future_socket + pal_socket/lwip_raw + common + 测试
OBJS+=" $(compile "$INC_AW" \
  "$AW/src/http_server.c" "$AW/src/sock/stream.c" \
  "$AW/src/tls/wolfssl.c" "$AW/src/sock/future_socket.c" \
  "$AW/src/sock/pal_socket/lwip_raw.c" "$AW/src/common.c" \
  "$QD/bare_tls_shim.c" "$QD/test_fr_https.c")"

$CC $CFLAGS --specs=rdimon.specs -T "$QD/mps2_an385.ld" -nostartfiles \
  $OBJS -o "$OUT" 2>&1 | grep -ivE "RWX permissions|LOAD segment" || true

echo "built: $OUT"
