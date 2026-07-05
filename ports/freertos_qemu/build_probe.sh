#!/usr/bin/env bash
# wolfSSL 裸机握手探针的构建 (独立于 build.sh)。
# 探针只需 wolfSSL + FreeRTOS bootstrap, 不接 lwIP/libcoro loop。
#
# 分两组编译成 .o 再链接:
#   A. FreeRTOS 内核 + startup + boot_fr (FreeRTOS 头 + 桩)
#   B. wolfSSL (ssl/tls/wolfcrypt, ECC-only 路径; -DWOLFSSL_USER_SETTINGS)
#   C. 探针本体 (wolfSSL 头)
set -e

OUT="${1:-/tmp/probe.elf}"
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT

LIBCORO="$(cd "$(dirname "$0")/../.." && pwd)"
QD="$LIBCORO/ports/freertos_qemu"
FR="$HOME/.cache/FreeRTOS-Kernel"
WOLF="$HOME/.cache/wolfssl-5.9.2"

CC=arm-none-eabi-gcc
CFLAGS="-mcpu=cortex-m3 -mthumb -g -O2 -ffreestanding -Wall"

INC_FR="-I$QD -I$FR/include -I$FR/portable/GCC/ARM_CM3"
# wolfSSL: user_settings.h 在 $QD, wolfSSL 头在 $WOLF
INC_WOLF="-I$QD -I$WOLF -DWOLFSSL_USER_SETTINGS"

compile() {
  local inc="$1"; shift
  for f in "$@"; do
    local o="$OBJ/$(echo "$f" | md5sum | cut -c1-12).o"
    $CC $CFLAGS $inc -c "$f" -o "$o"
    echo "$o"
  done
}

OBJS=""

# 组 A: FreeRTOS + startup + bootstrap (含 wolf_gen_seed/wolf_time 桩)
OBJS+=" $(compile "$INC_FR" \
  "$QD/startup_mps2.c" "$QD/boot_fr.c" \
  "$FR/tasks.c" "$FR/list.c" "$FR/queue.c" "$FR/timers.c" \
  "$FR/portable/GCC/ARM_CM3/port.c" "$FR/portable/MemMang/heap_4.c")"

# 组 B: wolfSSL (ECC + AES-GCM + SHA256 + TLS1.2/1.3 路径)
OBJS+=" $(compile "$INC_WOLF" \
  "$WOLF/src/ssl.c" "$WOLF/src/internal.c" "$WOLF/src/tls.c" \
  "$WOLF/src/tls13.c" "$WOLF/src/keys.c" "$WOLF/src/wolfio.c" \
  "$WOLF/wolfcrypt/src/asn.c" "$WOLF/wolfcrypt/src/coding.c" \
  "$WOLF/wolfcrypt/src/ecc.c" "$WOLF/wolfcrypt/src/aes.c" \
  "$WOLF/wolfcrypt/src/sha256.c" \
  "$WOLF/wolfcrypt/src/random.c" "$WOLF/wolfcrypt/src/hmac.c" \
  "$WOLF/wolfcrypt/src/hash.c" "$WOLF/wolfcrypt/src/wc_encrypt.c" \
  "$WOLF/wolfcrypt/src/wc_port.c" "$WOLF/wolfcrypt/src/memory.c" \
  "$WOLF/wolfcrypt/src/error.c" "$WOLF/wolfcrypt/src/logging.c" \
  "$WOLF/wolfcrypt/src/wolfmath.c" "$WOLF/wolfcrypt/src/sp_int.c" \
  "$WOLF/wolfcrypt/src/sp_c32.c" "$WOLF/wolfcrypt/src/kdf.c")"

# 组 C: 探针本体
OBJS+=" $(compile "$INC_WOLF" "$QD/probe_wolfssl.c")"

$CC $CFLAGS --specs=rdimon.specs -T "$QD/mps2_an385.ld" -nostartfiles \
  $OBJS -o "$OUT" 2>&1 | grep -ivE "RWX permissions|LOAD segment" || true

echo "built: $OUT"
