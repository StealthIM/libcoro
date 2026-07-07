#!/usr/bin/env bash
# 编译 FreeRTOS + lwIP + libcoro 到 QEMU mps2-an385。
# 用法: build.sh <echo_impl.c> <out.elf>
#
# 关键: libcoro 私有头 task.h 与 FreeRTOS task.h 同名, 同一编译命令里两组
# -I 无法共存。故按翻译单元分组编译成 .o, 每组用各自的 include 集, 最后统一
# 链接。分四组:
#   A. FreeRTOS 内核 + FreeRTOS bootstrap + pal_thread (FreeRTOS 头, 无 lwIP)
#   B. lwIP 栈 + FreeRTOS sys_arch (lwIP 头 + FreeRTOS 头, 无 libcoro 私有头)
#   C. libcoro 核心 + loop 后端 (libcoro 私有头 + lwIP 头, 无 FreeRTOS 头)
#   D. echo 测试实现 (libcoro + lwIP, 无 FreeRTOS 头)
set -e

ECHO_IMPL="${1:?需要 echo 实现 .c}"
OUT="${2:-/tmp/fw.elf}"
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT

LIBCORO="$(cd "$(dirname "$0")/../.." && pwd)"
QD="$LIBCORO/ports/freertos_qemu"
FR="$HOME/.cache/FreeRTOS-Kernel"
LWIP="$HOME/.cache/lwip"
LWIP_FR="$LWIP/contrib/ports/freertos"

CC=arm-none-eabi-gcc
CFLAGS="-mcpu=cortex-m3 -mthumb -g -O1 -ffreestanding -Wall -DLIBCORO_LWIP=1"

INC_FR="-I$QD -I$FR/include -I$FR/portable/GCC/ARM_CM3 -I$LIBCORO/include"
INC_LWIP="-I$QD -I$LWIP/src/include -I$LWIP_FR/include -I$FR/include -I$FR/portable/GCC/ARM_CM3"
INC_CORO="-I$QD -I$LIBCORO/include -I$LWIP/src/include -I$LWIP_FR/include"

compile() { # <incset> <src...>
  local inc="$1"; shift
  for f in "$@"; do
    local o="$OBJ/$(echo "$f" | md5sum | cut -c1-12).o"
    $CC $CFLAGS $inc -c "$f" -o "$o"
    echo "$o"
  done
}

OBJS=""

# 组 A: FreeRTOS 内核 + startup + bootstrap + pal_thread
OBJS+=" $(compile "$INC_FR" \
  "$QD/startup_mps2.c" "$QD/boot_fr.c" \
  "$LIBCORO/src/pal/thread_freertos.c" \
  "$FR/tasks.c" "$FR/list.c" "$FR/queue.c" "$FR/timers.c" \
  "$FR/portable/GCC/ARM_CM3/port.c" "$FR/portable/MemMang/heap_4.c")"

# 组 B: lwIP 栈 + FreeRTOS sys_arch
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
  "$LWIP/src/netif/ethernet.c" \
  "$LWIP_FR/sys_arch.c")"

# 组 C: libcoro 核心 + loop 后端 (lwip_select 复用)
OBJS+=" $(compile "$INC_CORO" \
  "$LIBCORO/src/loop/lwip_select.c" "$LIBCORO/src/offload.c" \
  "$LIBCORO/src/future.c" "$LIBCORO/src/task.c" "$LIBCORO/src/generator.c")"

# 组 D: echo 测试实现
OBJS+=" $(compile "$INC_CORO" "$ECHO_IMPL")"

# 链接
$CC $CFLAGS --specs=rdimon.specs -T "$QD/mps2_an385.ld" -nostartfiles \
  $OBJS -o "$OUT" 2>&1 | grep -ivE "RWX permissions|LOAD segment" || true

echo "built: $OUT"
