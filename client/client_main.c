#include "ui/ui.h"
#include "logic/client_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "用法: %s <服务器IP> <服务器端口> [P2P端口]\n", argv[0]);
        return 1;
    }

    // 1. 初始化核心服务
    char exe_path_buf[PATH_MAX];
    char final_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path_buf, sizeof(exe_path_buf) - 1);
    if (len != -1) {
        exe_path_buf[len] = '\0';
        strcpy(final_path, dirname(exe_path_buf));
    } else {
        getcwd(final_path, sizeof(final_path));
    }

    if (init_client_services(final_path) != 0) {
        fprintf(stderr, "客户端服务初始化失败。\n");
        return 1;
    }

    // 2. 初始化UI
    init_ui();

    // 3. 连接网络
    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int p2p_port = (argc == 4) ? atoi(argv[3]) : 0;
    if (connect_and_listen(server_ip, server_port, p2p_port) != 0) {
        log_msg("[致命错误] 网络连接失败。");
        sleep(3);
        destroy_ui();
        shutdown_client_services();
        return 1;
    }

    // 4. 进入主事件循环
    while (current_ui_state != UI_STATE_EXITING) {
        // 所有UI更新都在这个主线程中串行执行
        update_logs_from_queue();
        handle_input_and_events();
        usleep(50000); // 50ms延迟，防止CPU占用过高
    }

    // 5. 清理和退出
    destroy_ui();
    shutdown_client_services();

    printf("ZeroLink客户端已退出。\n");
    return 0;
}
