#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "libcoro.h"

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

static const char *host = "postman-echo.com";
static socket_t g_sock;

/*========================
  HTTP 请求发送完成回调
=========================*/
static void on_recv(
    loop_t *loop,
    void *userdata,
    loop_op_type_t _,
    int err,
    unsigned long bytes,
    recv_data_t *recv
);
static void on_send(
    loop_t *loop,
    void *userdata,
    loop_op_type_t _,
    int err,
    unsigned long bytes,
    recv_data_t *__
) {
    if (err != 0) {
        printf("send error: %d\n", err);
        loop_stop();
        return;
    }

    printf("send ok: %lu bytes\n", bytes);

    /* 继续投递 recv */
    char *buf = malloc(8192);
    loop_post_recv(
        (void*)(intptr_t)g_sock,
        buf,
        8192,
        on_recv,
        NULL
    );
}

/*========================
  HTTP 响应接收回调
=========================*/
static void on_recv(
    loop_t *loop,
    void *userdata,
    loop_op_type_t _,
    int err,
    unsigned long bytes,
    recv_data_t *recv
) {

    printf("----- recv %lu bytes with err %d -----\n", bytes, err);

    if (err != 0 || bytes == 0) {
        printf("recv finished\n");
        free(recv->data);
        return;
    }

    fwrite(recv->data, 1, bytes, stdout);
    free(recv->data);
    printf("\n");

    /* 继续接收后续数据 */
    char *buf = malloc(8192);
    loop_post_recv(
        (void*)(intptr_t)g_sock,
        buf,
        8192,
        on_recv,
        NULL
    );
}

/*========================
  connect 完成回调
=========================*/
static void on_connect(
    loop_t *loop,
    void *userdata,
    loop_op_type_t type,
    int err,
    unsigned long bytes,
    recv_data_t *_
) {
    if (err != 0) {
        printf("connect failed: %d\n", err);
        loop_stop();
        return;
    }

    printf("connect ok\n");

    /* 发送 HTTP 请求 */
    const char *path = "/get";
    static char request[512];

    snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n\r\n",
        path,
        host
    );

    printf("Data size: %llu\n", strlen(request));

    loop_post_send(
        (void*)(intptr_t)g_sock,
        request,
        strlen(request),
        on_send,
        NULL
    );
}


task_t* task(main_task) {
    gen_dec_vars();
    gen_begin(ctx);
    printf("Start\n");

    /* 解析域名 */
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, "80", &hints, &res) != 0) {
        printf("getaddrinfo failed\n");
        gen_return(1);
    }

    /* 创建 socket */
    g_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_sock == INVALID_SOCKET) {
        printf("socket failed\n");
        gen_return(1);
    }
    loop_bind_handle((void*) g_sock);

    /* 非阻塞 */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(g_sock, FIONBIO, &mode);
#else
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);
#endif
    loop_connect_async(
        (void*)(intptr_t)g_sock,
        res->ai_addr,
        (int)res->ai_addrlen,
        on_connect,
        NULL
    );
    printf("Wait\n");

    freeaddrinfo(res);
    gen_end(0);
}

/*========================
  测试主入口
=========================*/
int test_loop() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
#endif

    /* 启动 loop */
    task_t* maintask = main_task();
    loop_run(maintask);

    loop_destroy();
    closesocket(g_sock);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
