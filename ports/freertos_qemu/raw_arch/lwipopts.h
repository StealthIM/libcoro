#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H
/* raw (NO_SYS) 构建的 lwipopts 入口: 让 lwIP 的 #include "lwipopts.h" 命中
 * 裸机版。把 raw_arch/ 放在 include 路径最前, 使其 lwipopts.h + arch/cc.h
 * 覆盖 $QD 里的 FreeRTOS 版本。 */
#include "lwipopts_raw.h"
#endif
