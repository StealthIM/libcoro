// test_loop_http.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <libcoro.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <windows.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t;
#else
  #include <unistd.h>
  #include <errno.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <time.h>
  #include <fcntl.h>
  typedef int socket_t;
  #ifndef INVALID_SOCKET
    #define INVALID_SOCKET (-1)
  #endif
  #ifndef SOCKET_ERROR
    #define SOCKET_ERROR   (-1)
  #endif
  #define closesocket close
#endif

#include "tools.h"

const char *host = "postman-echo.com";

static void socket_cb(loop_t *_, void *userdata) {
    recv_data_t *data = userdata;

    data->data[data->len] = '\0'; // 确保字符串结束

    printf("Received %lu bytes:\n", data->len);
    printf("%s\n", data->data);
}

gen_ret_t main_test_loop(gen_ctx_t *ctx, void *arg) {
    gen_dec_vars(socket_t sock);
    gen_begin(ctx);
    gen_var(sock) = (socket_t)gen_userdata();
    printf("1\n");
    const char *path = "/get";
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    send(gen_var(sock), request, (int)strlen(request), 0);
    gen_yield(async_sleep(1000));
    printf("2\n");
    gen_yield(async_sleep(1000));
    printf("3\n");
    gen_end(0);
}

int test_loop() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    // 解析域名
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int ret = getaddrinfo(host, "80", &hints, &res);
    if (ret != 0 || !res) {
        fprintf(stderr, "getaddrinfo failed: %s\n",
#ifdef _WIN32
            gai_strerrorA(ret)
#else
            gai_strerror(ret)
#endif
        );
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // 创建 socket
    socket_t sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        perror("socket failed");
#ifdef _WIN32
        WSACleanup();
#endif
        freeaddrinfo(res);
        return 1;
    }

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        perror("connect failed");
        closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        freeaddrinfo(res);
        return 1;
    }
    freeaddrinfo(res);

    // 设置非阻塞
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    loop_register_handle((void*)(intptr_t)sock, socket_cb, NULL);
    gen_t *main = gen_create(main_test_loop, (void*)sock);
    task_t *task = task_create(main);
    loop_run(task);

    loop_destroy();

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
