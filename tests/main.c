#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int test_nested_task();
int test_empty_task();
int test_emit();
#ifndef LIBCORO_NO_OFFLOAD
int test_offload();
#endif

#ifdef LIBCORO_LWIP
/* lwip 后端: test_loop 用系统 socket + 外部网络, 与 lwIP 冲突且跑不了;
 * 改用纯栈内环回 echo 测试 (accept/connect/send/recv + UDP 自唤醒)。 */
int test_lwip_echo();
#else
int test_loop();
#endif

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("Usage: %s <testname>\n", argv[0]);
        return 1;
    }
#ifdef LIBCORO_LWIP
    if (strcmp(argv[1], "lwip_echo") == 0) return test_lwip_echo();
#else
    if (strcmp(argv[1], "loop") == 0) return test_loop();
#endif
    if (strcmp(argv[1], "nested_task") == 0) return test_nested_task();
    if (strcmp(argv[1], "empty_task") == 0) return test_empty_task();
    if (strcmp(argv[1], "emit") == 0) return test_emit();
#ifndef LIBCORO_NO_OFFLOAD
    if (strcmp(argv[1], "offload") == 0) return test_offload();
#endif

    printf("Unknown test: %s\n", argv[1]);
    return 1;
}
