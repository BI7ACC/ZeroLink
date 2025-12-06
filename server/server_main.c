#include "bootstrap/bootstrap_server.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "用法: %s <端口号>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "错误: 无效的端口号 %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    if (start_bootstrap_server(port) != 0) {
        fprintf(stderr, "启动引导服务器失败。\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}
