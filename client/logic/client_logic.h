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
void request_chat_sync(const char* friend_pk_hex);

// --- 新增的设置接口 ---
const char* get_my_public_key_hex();
int get_my_p2p_port();
int get_online_peer_count();

#endif //ZEROLINK_CLIENT_LOGIC_H
