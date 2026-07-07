#!/usr/bin/env bash
# 裸机 (NO_SYS=1) lwIP raw + libcoro 到 QEMU mps2-an385。
# 用法: build_raw.sh <out.elf>
#
# 本构建 (vs FreeRTOS 版):
#   - 无 FreeRTOS: boot_raw.c 直接 main->run_echo_loop, SysTick 做时基。
#   - 无 offload / pal_thread: 裸机单上下文, 不编线程池。
#   - lwIP 只编 core (NO_SYS): 不要 api/ (netconn/sockets/tcpip) 和 sys_arch。
#   - loop 后端用 lwip_raw.c (回调式), 非 lwip_select.c。
#   - raw_arch/ 放 include 路径最前, 其 lwipopts.h + arch/cc.h 覆盖 FreeRTOS 版。
#
# 无 task.h 同名冲突问题 (不含 FreeRTOS 头), 但仍分组保持清晰:
#   A. startup + 裸机 boot
#   B. lwIP core (NO_SYS, 无 api/, 无 sys_arch)
#   C. libcoro 核心 + lwip_raw loop 后端 (无 offload)
#   D. 测试实现
set -e

OUT="${1:-/tmp/fr_getsockname.elf}"
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT

LIBCORO="$(cd "$(dirname "$0")/../.." && pwd)"
QD="$LIBCORO/ports/freertos_qemu"
LWIP="$HOME/.cache/lwip"

CC=arm-none-eabi-gcc
CFLAGS="-mcpu=cortex-m3 -mthumb -g -O1 -ffreestanding -Wall -DLIBCORO_LWIP_RAW=1 ${LOOP_DEBUG:+-DLOOP_DEBUG}"

# raw_arch 必须在最前: 覆盖 $QD 的 FreeRTOS lwipopts.h / arch/cc.h。
INC_LWIP="-I$QD/raw_arch -I$LWIP/src/include"
INC_CORO="-I$QD/raw_arch -I$LIBCORO/include -I$LWIP/src/include -I$QD"

compile() { # <incset> <src...>
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

# 组 B: lwIP core (NO_SYS): 无 api/ (netconn/sockets/tcpip), 无 sys_arch。
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

# 组 C: libcoro 核心 + lwip_raw loop 后端 (无 offload)
OBJS+=" $(compile "$INC_CORO" \
  "$LIBCORO/src/loop/lwip_raw.c" \
  "$LIBCORO/src/future.c" "$LIBCORO/src/task.c" "$LIBCORO/src/generator.c")"

# 组 D: 测试实现
OBJS+=" $(compile "$INC_CORO" "$QD/test_fr_getsockname.c")"

# 链接
$CC $CFLAGS --specs=rdimon.specs -T "$QD/mps2_an385.ld" -nostartfiles \
  $OBJS -o "$OUT" 2>&1 | grep -ivE "RWX permissions|LOAD segment" || true

echo "built: $OUT"
