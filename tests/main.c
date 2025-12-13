#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int test_loop();
int test_nested_task();

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("Usage: %s <testname>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "loop") == 0) return test_loop();
    if (strcmp(argv[1], "nested_task") == 0) return test_nested_task();

    printf("Unknown test: %s\n", argv[1]);
    return 1;
}
