#include "bootstrap_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MAX_CLIENTS 30
#define BUFFER_SIZE 1024
#define PK_HEX_LEN 64 // crypto_box_PUBLICKEYBYTES * 2

// 客户端信息结构体
typedef struct {
    int sockfd;
    char ip[INET_ADDRSTRLEN];
    int p2p_port;
    char pk_hex[PK_HEX_LEN + 1]; // 客户端公钥的十六进制表示
} client_t;

// 全局客户端列表和互斥锁
static client_t *clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_client(client_t *cl) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) {
            clients[i] = cl;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void remove_client(int sockfd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->sockfd == sockfd) {
            free(clients[i]);
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void broadcast(const char *message, int sender_sockfd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->sockfd != sender_sockfd) {
            if (send(clients[i]->sockfd, message, strlen(message), 0) < 0) {
                perror("广播消息失败");
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void *handle_client(void *arg) {
    client_t *cli = (client_t *)arg;
    char buffer[BUFFER_SIZE];
    char line_buffer[BUFFER_SIZE];
    int n, line_len = 0;

    // 1. 读取客户端发来的注册信息: "<pk_hex> <p2p_port>\n"
    while ((n = recv(cli->sockfd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        for (int i = 0; i < n; i++) {
            if (buffer[i] == '\n') {
                line_buffer[line_len] = '\0';
                goto registration_received;
            }
            if (line_len < BUFFER_SIZE - 1) {
                line_buffer[line_len++] = buffer[i];
            }
        }
    }
    printf("客户端 %s 未发送注册信息，连接已断开。\n", cli->ip);
    fflush(stdout);
    close(cli->sockfd);
    free(cli);
    return NULL;

registration_received:
    if (sscanf(line_buffer, "%s %d", cli->pk_hex, &cli->p2p_port) != 2) {
        printf("客户端 %s 发送了无效的注册格式，连接已断开。\n", cli->ip);
        fflush(stdout);
        close(cli->sockfd);
        free(cli);
        return NULL;
    }
    printf("客户端 %s (公钥: %s...) 已在端口 %d 上注册。\n", cli->ip, strndup(cli->pk_hex, 8), cli->p2p_port);
    fflush(stdout);

    // 2. 将客户端自己的IP地址发回给它
    sprintf(buffer, "MY_IP %s\n", cli->ip);
    send(cli->sockfd, buffer, strlen(buffer), 0);

    // 3. 将已存在的其他客户端信息发送给当前新客户端
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->sockfd != cli->sockfd) {
            sprintf(buffer, "PEER %s %s %d\n", clients[i]->pk_hex, clients[i]->ip, clients[i]->p2p_port);
            send(cli->sockfd, buffer, strlen(buffer), 0);
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    // 4. 将新客户端的信息广播给所有其他客户端
    sprintf(buffer, "NEW_PEER %s %s %d\n", cli->pk_hex, cli->ip, cli->p2p_port);
    broadcast(buffer, cli->sockfd);

    // 5. 将客户端加入全局列表
    add_client(cli);

    // 6. 等待客户端断开连接
    while (recv(cli->sockfd, buffer, BUFFER_SIZE - 1, 0) > 0) {}

    // 7. 客户端断开连接
    close(cli->sockfd);
    printf("客户端 %s (公钥: %s...) 已断开连接。\n", cli->ip, strndup(cli->pk_hex, 8));
    fflush(stdout);
    
    // 8. 广播客户端离开的消息
    sprintf(buffer, "DEL_PEER %s\n", cli->pk_hex);
    broadcast(buffer, -1);

    // 9. 从列表中移除
    remove_client(cli->sockfd);

    return NULL;
}

int start_bootstrap_server(int port) {
    int listen_fd;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t cli_len = sizeof(cli_addr);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("创建socket失败");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("绑定端口失败");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 10) < 0) {
        perror("监听失败");
        close(listen_fd);
        return -1;
    }
    
    printf("引导服务器正在端口 %d 上监听...\n", port);
    fflush(stdout);

    while (1) {
        int conn_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (conn_fd < 0) {
            perror("接受连接失败");
            continue;
        }

        client_t *cli = (client_t *)malloc(sizeof(client_t));
        cli->sockfd = conn_fd;
        inet_ntop(AF_INET, &cli_addr.sin_addr, cli->ip, INET_ADDRSTRLEN);
        
        pthread_t tid;
        pthread_create(&tid, NULL, &handle_client, (void*)cli);
        pthread_detach(tid);
    }

    close(listen_fd);
    return 0;
}
