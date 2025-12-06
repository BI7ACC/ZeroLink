#ifndef ZEROLINK_CLIENT_LOGIC_H
#define ZEROLINK_CLIENT_LOGIC_H

#include "../../core/models/friend.h"
#include <sqlite3.h>

/**
 * @file client_logic.h
 * @brief 客户端核心业务逻辑接口。
 */

int init_client_services(const char* exe_path);
void shutdown_client_services();
int connect_and_listen(const char* server_ip, int server_port, int p2p_port);
void send_chat_message(const char* recipient_name, const char* message);
void add_new_friend(const char* pk_hex, const char* name);
void delete_friend_by_name(const char* name);
friend_t** get_friends();
int get_friend_count();
void db_load_history(const char* chat_id);
void log_msg(const char *format, ...);

/**
 * @brief 向指定的在线好友请求同步聊天记录。
 * @param friend_pk_hex 好友的公钥十六进制字符串。
 */
void request_chat_sync(const char* friend_pk_hex);

#endif //ZEROLINK_CLIENT_LOGIC_H
