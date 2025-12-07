#include "client_logic.h"
#include "../ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sodium.h>
#include <libgen.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <sqlite3.h>
#include <cjson/cJSON.h>
#include <limits.h>

#define MAX_PEERS 30
#define MAX_FRIENDS 50
#define BUFFER_SIZE 4096
#define IDENTITY_FILE "identity.dat"
#define FRIENDS_FILE "friends.dat"
#define DB_FILE "chat.db"

typedef struct {
    int sockfd;
    char ip[INET_ADDRSTRLEN];
    int port;
    unsigned char pk[crypto_box_PUBLICKEYBYTES];
    unsigned char shared_key[crypto_box_BEFORENMBYTES];
    int key_exchanged;
    pthread_t recv_tid;
} peer_t;

// --- 全局变量与锁 ---
static sqlite3 *db;
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;
static friend_t* friends[MAX_FRIENDS];
static int friend_count = 0;
static unsigned char my_pk[crypto_box_PUBLICKEYBYTES];
static unsigned char my_sk[crypto_box_SECRETKEYBYTES];
static char my_pk_hex[PK_HEX_LEN + 1]; // 缓存十六进制公钥
static char exe_dir[PATH_MAX];
static peer_t *peers[MAX_PEERS];
static pthread_mutex_t peers_mutex = PTHREAD_MUTEX_INITIALIZER;
static char my_ip[INET_ADDRSTRLEN] = {0};
static int my_p2p_port = 0;
static pthread_mutex_t port_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t port_cond = PTHREAD_COND_INITIALIZER;
static int port_ready = 0;

// --- 内部函数原型 ---
static void init_identity();
static void load_friends();
static void save_friends();
static void db_init();
static const char* get_config_path(const char* filename, char* out_path, size_t out_len);
static const char* get_friend_pk_by_name(const char* name);
static const char* get_friend_name_by_hex(const char *pk_hex);
static const char* get_friend_name(const unsigned char *pk);
static void generate_message_uid(char* uid_buf, size_t buf_len);
static void db_save_message(const char* message_uid, const char* chat_id, const char* sender_pk_hex, const char* content, const char* vector_clock);
static void send_encrypted(int sockfd, const unsigned char* shared_key, const char* json_string);
static void *p2p_listener(void *arg);
static void *server_handler(void *arg);
static cJSON* db_get_vector_clock(const char* chat_id);
static void db_save_vector_clock(const char* chat_id, cJSON* clock);
static void vc_merge(cJSON* local_clock, cJSON* remote_clock);
static void vc_increment(cJSON* clock, const char* node_id);
static void add_peer(peer_t *peer);
static void remove_peer(int sockfd);
static void connect_to_peer(const char *pk_hex, const char *ip, int port);

// --- 日志 ---
void log_msg(const char *format, ...) {
    char buffer[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    queue_log_message(buffer);
}

// --- 服务初始化/关闭 ---
static const char* get_config_path(const char* filename, char* out_path, size_t out_len) {
    snprintf(out_path, out_len, "%s/%s", exe_dir, filename);
    return out_path;
}

int init_client_services(const char* path) {
    strcpy(exe_dir, path);
    if (sodium_init() < 0) {
        fprintf(stderr, "致命错误: libsodium 初始化失败！\n");
        return -1;
    }
    db_init();
    init_identity();
    load_friends();
    return 0;
}

void shutdown_client_services() {
    if (db) sqlite3_close(db);
    for (int i = 0; i < friend_count; i++) free(friends[i]);
    for (int i = 0; i < MAX_PEERS; i++) {
        if(peers[i]) {
            close(peers[i]->sockfd);
            free(peers[i]);
        }
    }
}

// --- 数据库操作 (全部加锁) ---
static void db_init() {
    char path[PATH_MAX];
    get_config_path(DB_FILE, path, sizeof(path));
    if (sqlite3_open(path, &db)) {
        log_msg("[致命错误] 无法打开数据库: %s", sqlite3_errmsg(db));
        exit(1);
    }
    char *err_msg = 0;
    const char *sql_messages = "CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY, message_uid TEXT UNIQUE, chat_id TEXT, sender_pk TEXT, content TEXT, timestamp INTEGER, vector_clock TEXT);";
    if (sqlite3_exec(db, sql_messages, 0, 0, &err_msg) != SQLITE_OK) {
        log_msg("[致命错误] 无法创建消息表: %s", err_msg);
        sqlite3_free(err_msg);
        exit(1);
    }
    const char *sql_vc = "CREATE TABLE IF NOT EXISTS vector_clocks(chat_id TEXT PRIMARY KEY, clock TEXT);";
    if (sqlite3_exec(db, sql_vc, 0, 0, &err_msg) != SQLITE_OK) {
        log_msg("[致命错误] 无法创建向量时钟表: %s", err_msg);
        sqlite3_free(err_msg);
        exit(1);
    }
}

static void db_save_message(const char* message_uid, const char* chat_id, const char* sender_pk_hex, const char* content, const char* vector_clock) {
    pthread_mutex_lock(&db_mutex);
    char *sql = sqlite3_mprintf("INSERT OR IGNORE INTO messages (message_uid, chat_id, sender_pk, content, timestamp, vector_clock) VALUES ('%q', '%q', '%q', '%q', %lld, '%q');",
                          message_uid, chat_id, sender_pk_hex, content, (sqlite3_int64)time(NULL), vector_clock ? vector_clock : "");
    char *err_msg = 0;
    if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK) {
        log_msg("[数据库错误] 保存消息失败: %s", err_msg);
        sqlite3_free(err_msg);
    }
    sqlite3_free(sql);
    pthread_mutex_unlock(&db_mutex);
}

void db_load_history(const char* chat_id) {
    pthread_mutex_lock(&db_mutex);
    char* sql = sqlite3_mprintf("SELECT sender_pk, content FROM messages WHERE chat_id = '%q' ORDER BY timestamp ASC LIMIT 50;", chat_id);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *sender_pk_hex = (const char*)sqlite3_column_text(stmt, 0);
            const char *content = (const char*)sqlite3_column_text(stmt, 1);
            char my_pk_hex_local[PK_HEX_LEN + 1];
            sodium_bin2hex(my_pk_hex_local, sizeof(my_pk_hex_local), my_pk, sizeof(my_pk));
            char buffer[BUFFER_SIZE];
            if (strcmp(sender_pk_hex, my_pk_hex_local) == 0) {
                snprintf(buffer, sizeof(buffer), "[我]: %s", content);
            } else {
                snprintf(buffer, sizeof(buffer), "[%s]: %s", get_friend_name_by_hex(sender_pk_hex), content);
            }
            log_msg(buffer);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_free(sql);
    pthread_mutex_unlock(&db_mutex);
}

static cJSON* db_get_vector_clock(const char* chat_id) {
    pthread_mutex_lock(&db_mutex);
    sqlite3_stmt *stmt;
    char* sql = sqlite3_mprintf("SELECT clock FROM vector_clocks WHERE chat_id = '%q';", chat_id);
    cJSON* clock_json = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *clock_str = (const char*)sqlite3_column_text(stmt, 0);
            if(clock_str) clock_json = cJSON_Parse(clock_str);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_free(sql);
    pthread_mutex_unlock(&db_mutex);
    return clock_json ? clock_json : cJSON_CreateObject();
}

static void db_save_vector_clock(const char* chat_id, cJSON* clock) {
    char* clock_str = cJSON_PrintUnformatted(clock);
    if (!clock_str) return;
    pthread_mutex_lock(&db_mutex);
    char* sql = sqlite3_mprintf("INSERT OR REPLACE INTO vector_clocks (chat_id, clock) VALUES ('%q', '%q');", chat_id, clock_str);
    char *err_msg = 0;
    if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK) {
        log_msg("[数据库错误] 保存向量时钟失败: %s", err_msg);
        sqlite3_free(err_msg);
    }
    sqlite3_free(sql);
    pthread_mutex_unlock(&db_mutex);
    free(clock_str);
}

// --- 向量时钟 (无锁，由调用者保证) ---
static void vc_increment(cJSON* clock, const char* node_id) {
    cJSON* node_clock = cJSON_GetObjectItem(clock, node_id);
    if (cJSON_IsNumber(node_clock)) {
        cJSON_SetNumberValue(node_clock, node_clock->valuedouble + 1);
    } else {
        cJSON_AddNumberToObject(clock, node_id, 1);
    }
}

static void vc_merge(cJSON* local_clock, cJSON* remote_clock) {
    cJSON *remote_node;
    cJSON_ArrayForEach(remote_node, remote_clock) {
        const char* node_id = remote_node->string;
        cJSON* local_node = cJSON_GetObjectItem(local_clock, node_id);
        if (cJSON_IsNumber(local_node)) {
            if (remote_node->valuedouble > local_node->valuedouble) {
                cJSON_SetNumberValue(local_node, remote_node->valuedouble);
            }
        } else {
            cJSON_AddNumberToObject(local_clock, node_id, remote_node->valuedouble);
        }
    }
}

// --- 身份与好友管理 ---
static void init_identity() {
    char path[PATH_MAX];
    get_config_path(IDENTITY_FILE, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fread(my_pk, 1, sizeof(my_pk), fp);
        fread(my_sk, 1, sizeof(my_sk), fp);
        fclose(fp);
    } else {
        log_msg("[身份] 未找到身份文件。正在生成新身份...");
        crypto_box_keypair(my_pk, my_sk);
        fp = fopen(path, "wb");
        if (fp) {
            fwrite(my_pk, 1, sizeof(my_pk), fp);
            fwrite(my_sk, 1, sizeof(my_sk), fp);
            fclose(fp);
        } else {
            log_msg("[错误] 保存身份文件失败: %s", strerror(errno));
            exit(1);
        }
    }
    sodium_bin2hex(my_pk_hex, sizeof(my_pk_hex), my_pk, sizeof(my_pk));
    log_msg("==================================================================");
    log_msg("您的公钥 (ID): %s", my_pk_hex);
    log_msg("==================================================================");
}

static void load_friends() {
    char path[PATH_MAX];
    get_config_path(FRIENDS_FILE, path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[BUFFER_SIZE];
    while (friend_count < MAX_FRIENDS && fgets(line, sizeof(line), fp)) {
        char *comma = strchr(line, ',');
        if (comma) {
            *comma = '\0';
            char *name = comma + 1;
            name[strcspn(name, "\n")] = 0;
            friends[friend_count] = malloc(sizeof(friend_t));
            strncpy(friends[friend_count]->pk_hex, line, PK_HEX_LEN);
            friends[friend_count]->pk_hex[PK_HEX_LEN] = '\0';
            strncpy(friends[friend_count]->name, name, 31);
            friends[friend_count]->name[31] = '\0';
            friend_count++;
        }
    }
    fclose(fp);
    log_msg("[好友] 已加载 %d 位好友。", friend_count);
}

static void save_friends() {
    char path[PATH_MAX];
    get_config_path(FRIENDS_FILE, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < friend_count; i++) {
        fprintf(fp, "%s,%s\n", friends[i]->pk_hex, friends[i]->name);
    }
    fclose(fp);
}

void add_new_friend(const char* pk_hex, const char* name) {
    if (friend_count >= MAX_FRIENDS) {
        log_msg("[系统] 错误: 好友列表已满。");
        return;
    }
    friends[friend_count] = malloc(sizeof(friend_t));
    strcpy(friends[friend_count]->pk_hex, pk_hex);
    strcpy(friends[friend_count]->name, name);
    friend_count++;
    save_friends();
    log_msg("[系统] 好友 %s 已添加。", name);
}

void delete_friend_by_name(const char* name) {
    int found_idx = -1;
    for (int i = 0; i < friend_count; i++) {
        if (strcmp(friends[i]->name, name) == 0) {
            found_idx = i;
            break;
        }
    }
    if (found_idx != -1) {
        free(friends[found_idx]);
        for (int i = found_idx; i < friend_count - 1; i++) {
            friends[i] = friends[i + 1];
        }
        friend_count--;
        save_friends();
        log_msg("[系统] 好友 %s 已删除。", name);
    }
}

friend_t** get_friends() { return friends; }
int get_friend_count() { return friend_count; }

static const char* get_friend_pk_by_name(const char* name) {
    for (int i = 0; i < friend_count; i++) {
        if (strcmp(friends[i]->name, name) == 0) return friends[i]->pk_hex;
    }
    return NULL;
}

static const char* get_friend_name_by_hex(const char *pk_hex) {
    for (int i = 0; i < friend_count; i++) {
        if (strcmp(friends[i]->pk_hex, pk_hex) == 0) return friends[i]->name;
    }
    return "未知用户";
}

static const char* get_friend_name(const unsigned char *pk) {
    char pk_hex[PK_HEX_LEN + 1];
    sodium_bin2hex(pk_hex, sizeof(pk_hex), pk, crypto_box_PUBLICKEYBYTES);
    return get_friend_name_by_hex(pk_hex);
}

static int is_friend(const char *pk_hex) {
    for (int i = 0; i < friend_count; i++) {
        if (strcmp(friends[i]->pk_hex, pk_hex) == 0) return 1;
    }
    return 0;
}

// --- 消息与网络核心逻辑 ---
static void generate_message_uid(char* uid_buf, size_t buf_len) {
    char my_pk_hex_local[PK_HEX_LEN + 1];
    unsigned char random_bytes[16];
    char random_hex[33];
    sodium_bin2hex(my_pk_hex_local, sizeof(my_pk_hex_local), my_pk, sizeof(my_pk));
    randombytes_buf(random_bytes, sizeof(random_bytes));
    sodium_bin2hex(random_hex, sizeof(random_hex), random_bytes, sizeof(random_bytes));
    snprintf(uid_buf, buf_len, "%s-%lld-%s", my_pk_hex_local, (long long)time(NULL), random_hex);
}

static void send_encrypted(int sockfd, const unsigned char* shared_key, const char* json_string) {
    size_t message_len = strlen(json_string);
    size_t ciphertext_len = crypto_box_MACBYTES + message_len;
    unsigned char nonce[crypto_box_NONCEBYTES];
    unsigned char *buffer = malloc(crypto_box_NONCEBYTES + ciphertext_len);
    if (!buffer) return;
    randombytes_buf(nonce, sizeof(nonce));
    if (crypto_box_easy_afternm(buffer + crypto_box_NONCEBYTES, (const unsigned char*)json_string, message_len, nonce, shared_key) != 0) {
        free(buffer);
        return;
    }
    memcpy(buffer, nonce, sizeof(nonce));
    send(sockfd, buffer, crypto_box_NONCEBYTES + ciphertext_len, 0);
    free(buffer);
}

void send_chat_message(const char* recipient_name, const char* message) {
    const char* target_pk_hex = get_friend_pk_by_name(recipient_name);
    if (!target_pk_hex) {
        log_msg("[系统] 错误：未在好友列表中找到名为 '%s' 的好友。", recipient_name);
        return;
    }
    char my_pk_hex_local[PK_HEX_LEN+1];
    sodium_bin2hex(my_pk_hex_local, sizeof(my_pk_hex_local), my_pk, sizeof(my_pk));
    char uid[PK_HEX_LEN + 50];
    generate_message_uid(uid, sizeof(uid));
    cJSON* clock = db_get_vector_clock(target_pk_hex);
    vc_increment(clock, my_pk_hex_local);
    char* clock_str = cJSON_PrintUnformatted(clock);
    
    db_save_message(uid, target_pk_hex, my_pk_hex_local, message, clock_str);
    db_save_vector_clock(target_pk_hex, clock);
    
    log_msg("[我 -> %s]: %s", recipient_name, message);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "chat");
    cJSON_AddStringToObject(json, "uid", uid);
    cJSON_AddStringToObject(json, "content", message);
    cJSON_AddStringToObject(json, "vector_clock", clock_str);
    char *json_string = cJSON_PrintUnformatted(json);
    
    free(clock_str);
    cJSON_Delete(clock);
    
    unsigned char target_pk[crypto_box_PUBLICKEYBYTES];
    sodium_hex2bin(target_pk, sizeof(target_pk), target_pk_hex, strlen(target_pk_hex), NULL, NULL, NULL);
    
    int found = 0;
    pthread_mutex_lock(&peers_mutex);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i] && peers[i]->key_exchanged && sodium_compare(peers[i]->pk, target_pk, sizeof(target_pk)) == 0) {
            send_encrypted(peers[i]->sockfd, peers[i]->shared_key, json_string);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&peers_mutex);
    
    free(json_string);
    if (!found) {
        log_msg("[系统] 提示：好友 %s 当前不在线，消息已缓存。", recipient_name);
    }
}

static void handle_sync_request(peer_t *peer, cJSON *json);
static void handle_sync_response(cJSON *json);

static void *receive_from_peer(void *arg) {
    peer_t *peer = (peer_t *)arg;
    unsigned char encrypted_buffer[BUFFER_SIZE];
    unsigned char decrypted_buffer[BUFFER_SIZE];
    int n;
    char sender_pk_hex[PK_HEX_LEN + 1];
    sodium_bin2hex(sender_pk_hex, sizeof(sender_pk_hex), peer->pk, sizeof(peer->pk));
    while ((n = recv(peer->sockfd, encrypted_buffer, sizeof(encrypted_buffer), 0)) > 0) {
        if (n < crypto_box_NONCEBYTES + crypto_box_MACBYTES) continue;
        if (crypto_box_open_easy_afternm(decrypted_buffer, encrypted_buffer + crypto_box_NONCEBYTES, n - crypto_box_NONCEBYTES, encrypted_buffer, peer->shared_key) != 0) continue;
        size_t decrypted_len = n - crypto_box_NONCEBYTES - crypto_box_MACBYTES;
        decrypted_buffer[decrypted_len] = '\0';
        cJSON *received_json = cJSON_Parse((const char*)decrypted_buffer);
        if (!received_json) continue;
        cJSON *type = cJSON_GetObjectItem(received_json, "type");
        if (!cJSON_IsString(type)) {
            cJSON_Delete(received_json);
            continue;
        }
        if (strcmp(type->valuestring, "chat") == 0) {
            cJSON *uid = cJSON_GetObjectItem(received_json, "uid");
            cJSON *content = cJSON_GetObjectItem(received_json, "content");
            cJSON *vc_str_item = cJSON_GetObjectItem(received_json, "vector_clock");
            if (cJSON_IsString(uid) && cJSON_IsString(content) && cJSON_IsString(vc_str_item)) {
                db_save_message(uid->valuestring, sender_pk_hex, sender_pk_hex, content->valuestring, vc_str_item->valuestring);
                cJSON* remote_clock = cJSON_Parse(vc_str_item->valuestring);
                if(remote_clock) {
                    cJSON* local_clock = db_get_vector_clock(sender_pk_hex);
                    vc_merge(local_clock, remote_clock);
                    db_save_vector_clock(sender_pk_hex, local_clock);
                    cJSON_Delete(local_clock);
                    cJSON_Delete(remote_clock);
                }
                if (current_ui_state == UI_STATE_CHATTING && strcmp(sender_pk_hex, chat_target_pk_hex) == 0) {
                    log_msg("[%s]: %s", get_friend_name_by_hex(sender_pk_hex), content->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "sync_request") == 0) {
            handle_sync_request(peer, received_json);
        } else if (strcmp(type->valuestring, "sync_response") == 0) {
            handle_sync_response(received_json);
        }
        cJSON_Delete(received_json);
    }
    remove_peer(peer->sockfd);
    return NULL;
}

static void add_peer(peer_t *peer) {
    pthread_mutex_lock(&peers_mutex);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i]) {
            peers[i] = peer;
            break;
        }
    }
    pthread_mutex_unlock(&peers_mutex);
    
    char pk_hex[PK_HEX_LEN + 1];
    sodium_bin2hex(pk_hex, sizeof(pk_hex), peer->pk, sizeof(peer->pk));
    
    log_msg("[系统] 好友 %s 已连接。", get_friend_name_by_hex(pk_hex));
    request_chat_sync(pk_hex);
}

static void remove_peer(int sockfd) {
    char friend_name_copy[32] = "未知用户";
    pthread_mutex_lock(&peers_mutex);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i] && peers[i]->sockfd == sockfd) {
            strcpy(friend_name_copy, get_friend_name(peers[i]->pk));
            close(peers[i]->sockfd);
            free(peers[i]);
            peers[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&peers_mutex);
    log_msg("[系统] 好友 %s 已断开连接。", friend_name_copy);
}

static void *p2p_listener(void *arg) {
    int requested_port = *(int*)arg;
    free(arg); 
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(requested_port);
    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        log_msg("[致命错误] P2P监听端口绑定失败: %s", strerror(errno));
        exit(1);
    }
    if (requested_port == 0) {
        socklen_t len = sizeof(serv_addr);
        getsockname(listen_fd, (struct sockaddr *)&serv_addr, &len);
    }
    pthread_mutex_lock(&port_mutex);
    my_p2p_port = ntohs(serv_addr.sin_port);
    port_ready = 1;
    pthread_cond_signal(&port_cond);
    pthread_mutex_unlock(&port_mutex);
    listen(listen_fd, 5);
    log_msg("[系统] P2P服务正在端口 %d 上监听...", my_p2p_port);
    while(1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int conn_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (conn_fd < 0) continue;
        unsigned char received_pk[crypto_box_PUBLICKEYBYTES];
        if (recv(conn_fd, received_pk, sizeof(received_pk), 0) != sizeof(received_pk)) {
            close(conn_fd);
            continue;
        }
        char pk_hex[PK_HEX_LEN + 1];
        sodium_bin2hex(pk_hex, sizeof(pk_hex), received_pk, sizeof(received_pk));
        if (!is_friend(pk_hex)) {
            close(conn_fd);
            continue;
        }
        peer_t *new_peer = (peer_t*)malloc(sizeof(peer_t));
        new_peer->sockfd = conn_fd;
        memcpy(new_peer->pk, received_pk, sizeof(received_pk));
        inet_ntop(AF_INET, &cli_addr.sin_addr, new_peer->ip, INET_ADDRSTRLEN);
        new_peer->port = ntohs(cli_addr.sin_port);
        if (crypto_box_beforenm(new_peer->shared_key, new_peer->pk, my_sk) != 0) {
            close(conn_fd);
            free(new_peer);
            continue;
        }
        new_peer->key_exchanged = 1;
        pthread_create(&new_peer->recv_tid, NULL, receive_from_peer, new_peer);
        pthread_detach(new_peer->recv_tid);
        add_peer(new_peer);
    }
    close(listen_fd);
    return NULL;
}

static void *server_handler(void *arg) {
    int server_sockfd = *(int*)arg;
    free(arg);
    char buffer[BUFFER_SIZE * 2] = {0};
    int len = 0;
    while (1) {
        int n = recv(server_sockfd, buffer + len, sizeof(buffer) - len - 1, 0);
        if (n <= 0) break;
        len += n;
        buffer[len] = '\0';
        char *line = buffer;
        while (1) {
            char *next_line = strchr(line, '\n');
            if (!next_line) break;
            *next_line = '\0';
            char cmd[32], pk_hex[PK_HEX_LEN + 1], ip[INET_ADDRSTRLEN];
            int port;
            if (sscanf(line, "MY_IP %s", ip) == 1) {
                strcpy(my_ip, ip);
            } else if (sscanf(line, "%s %s %s %d", cmd, pk_hex, ip, &port) == 4) {
                if (!is_friend(pk_hex)) {
                    line = next_line + 1;
                    continue;
                }
                if ((strcmp(cmd, "PEER") == 0 || strcmp(cmd, "NEW_PEER") == 0) && strlen(my_ip) > 0) {
                    char my_addr[PK_HEX_LEN + 30], peer_addr[PK_HEX_LEN + 30];
                    char my_pk_hex_local[PK_HEX_LEN + 1];
                    sodium_bin2hex(my_pk_hex_local, sizeof(my_pk_hex_local), my_pk, sizeof(my_pk));
                    sprintf(my_addr, "%s:%s:%d", my_pk_hex_local, my_ip, my_p2p_port);
                    sprintf(peer_addr, "%s:%s:%d", pk_hex, ip, port);
                    if (strcmp(my_addr, peer_addr) < 0) {
                         log_msg("[系统] 发现好友 %s，正在尝试连接...", get_friend_name_by_hex(pk_hex));
                         connect_to_peer(pk_hex, ip, port);
                    }
                }
            }
            line = next_line + 1;
        }
        len = strlen(line);
        memmove(buffer, line, len + 1);
    }
    log_msg("[系统] 与引导服务器的连接已断开。");
    close(server_sockfd);
    return NULL;
}

static void connect_to_peer(const char *pk_hex, const char *ip, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return;
    struct sockaddr_in peer_addr = {0};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &peer_addr.sin_addr) <= 0) {
        close(sockfd);
        return;
    }
    if (connect(sockfd, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
        close(sockfd);
        return;
    }
    send(sockfd, my_pk, sizeof(my_pk), 0);
    peer_t *new_peer = (peer_t*)malloc(sizeof(peer_t));
    new_peer->sockfd = sockfd;
    strcpy(new_peer->ip, ip);
    new_peer->port = port;
    sodium_hex2bin(new_peer->pk, sizeof(new_peer->pk), pk_hex, strlen(pk_hex), NULL, NULL, NULL);
    if (crypto_box_beforenm(new_peer->shared_key, new_peer->pk, my_sk) != 0) {
        close(sockfd);
        free(new_peer);
        return;
    }
    new_peer->key_exchanged = 1;
    pthread_create(&new_peer->recv_tid, NULL, receive_from_peer, new_peer);
    pthread_detach(new_peer->recv_tid);
    add_peer(new_peer);
}

int connect_and_listen(const char* server_ip, int server_port, int p2p_port) {
    pthread_t p2p_tid, server_tid;
    int *p2p_port_ptr = malloc(sizeof(int)); *p2p_port_ptr = p2p_port;
    pthread_create(&p2p_tid, NULL, p2p_listener, p2p_port_ptr);
    pthread_detach(p2p_tid);
    pthread_mutex_lock(&port_mutex);
    while (!port_ready) {
        pthread_cond_wait(&port_cond, &port_mutex);
    }
    pthread_mutex_unlock(&port_mutex);
    int server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);
    if (connect(server_sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        log_msg("[致命错误] 连接引导服务器失败: %s", strerror(errno));
        return -1;
    }
    log_msg("[系统] 已连接到引导服务器。");
    char registration_msg[PK_HEX_LEN + 20];
    char my_pk_hex_local[PK_HEX_LEN + 1];
    sodium_bin2hex(my_pk_hex_local, sizeof(my_pk_hex_local), my_pk, sizeof(my_pk));
    sprintf(registration_msg, "%s %d\n", my_pk_hex_local, my_p2p_port);
    send(server_sockfd, registration_msg, strlen(registration_msg), 0);
    int *server_sockfd_ptr = malloc(sizeof(int)); *server_sockfd_ptr = server_sockfd;
    pthread_create(&server_tid, NULL, server_handler, server_sockfd_ptr);
    pthread_detach(server_tid);
    return 0;
}

void request_chat_sync(const char* friend_pk_hex) {
    if (!friend_pk_hex) return;
    unsigned char target_pk[crypto_box_PUBLICKEYBYTES];
    sodium_hex2bin(target_pk, sizeof(target_pk), friend_pk_hex, strlen(friend_pk_hex), NULL, NULL, NULL);

    int sockfd = -1;
    unsigned char shared_key[crypto_box_BEFORENMBYTES];

    pthread_mutex_lock(&peers_mutex);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i] && peers[i]->key_exchanged && sodium_compare(peers[i]->pk, target_pk, sizeof(target_pk)) == 0) {
            sockfd = peers[i]->sockfd;
            memcpy(shared_key, peers[i]->shared_key, sizeof(shared_key));
            break; 
        }
    }
    pthread_mutex_unlock(&peers_mutex);

    if (sockfd != -1) {
        cJSON* local_clock = db_get_vector_clock(friend_pk_hex);
        char* local_clock_str = cJSON_PrintUnformatted(local_clock);
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "type", "sync_request");
        cJSON_AddStringToObject(json, "vector_clock", local_clock_str);
        char *json_string = cJSON_PrintUnformatted(json);
        
        send_encrypted(sockfd, shared_key, json_string);
        
        cJSON_Delete(json);
        free(json_string);
        cJSON_Delete(local_clock);
        free(local_clock_str);
        log_msg("[同步] 已向 %s 发送同步请求...", get_friend_name_by_hex(friend_pk_hex));
    } else {
        log_msg("[同步] 无法发送请求: %s 不在线。", get_friend_name_by_hex(friend_pk_hex));
    }
}

static void handle_sync_request(peer_t *peer, cJSON *json) {
    cJSON *remote_clock_item = cJSON_GetObjectItem(json, "vector_clock");
    if (!cJSON_IsString(remote_clock_item)) return;
    cJSON* remote_clock = cJSON_Parse(remote_clock_item->valuestring);
    if (!remote_clock) return;

    char peer_pk_hex[PK_HEX_LEN + 1];
    sodium_bin2hex(peer_pk_hex, sizeof(peer_pk_hex), peer->pk, sizeof(peer->pk));

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "type", "sync_response");
    cJSON *messages_to_send = cJSON_CreateArray();
    cJSON_AddItemToObject(response, "messages", messages_to_send);

    pthread_mutex_lock(&db_mutex);
    char* sql = sqlite3_mprintf("SELECT sender_pk, vector_clock, content, message_uid, timestamp FROM messages WHERE chat_id = '%q';", peer_pk_hex);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *sender_pk = (const char*)sqlite3_column_text(stmt, 0);
            const char *msg_vc_str = (const char*)sqlite3_column_text(stmt, 1);
            if (!msg_vc_str || msg_vc_str[0] == '\0') continue;
            cJSON* msg_vc = cJSON_Parse(msg_vc_str);
            if (!msg_vc) continue;
            cJSON* msg_sender_entry = cJSON_GetObjectItem(msg_vc, sender_pk);
            if (!cJSON_IsNumber(msg_sender_entry)) {
                cJSON_Delete(msg_vc);
                continue;
            }
            cJSON* remote_sender_entry = cJSON_GetObjectItem(remote_clock, sender_pk);
            if (!remote_sender_entry || msg_sender_entry->valuedouble > cJSON_GetNumberValue(remote_sender_entry)) {
                 cJSON *msg_obj = cJSON_CreateObject();
                 cJSON_AddStringToObject(msg_obj, "uid", (const char*)sqlite3_column_text(stmt, 3));
                 cJSON_AddStringToObject(msg_obj, "sender_pk", sender_pk);
                 cJSON_AddStringToObject(msg_obj, "content", (const char*)sqlite3_column_text(stmt, 2));
                 cJSON_AddNumberToObject(msg_obj, "timestamp", sqlite3_column_int64(stmt, 4));
                 cJSON_AddStringToObject(msg_obj, "vector_clock", msg_vc_str);
                 cJSON_AddItemToArray(messages_to_send, msg_obj);
            }
            cJSON_Delete(msg_vc);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_free(sql);
    pthread_mutex_unlock(&db_mutex);

    if (cJSON_GetArraySize(messages_to_send) > 0) {
        char *response_str = cJSON_PrintUnformatted(response);
        send_encrypted(peer->sockfd, peer->shared_key, response_str);
        free(response_str);
        log_msg("[同步] 向 %s 发送了 %d 条缺失的消息。", get_friend_name(peer->pk), cJSON_GetArraySize(messages_to_send));
    }

    cJSON_Delete(response);
    cJSON_Delete(remote_clock);
}

static void handle_sync_response(cJSON *json) {
    cJSON *messages = cJSON_GetObjectItem(json, "messages");
    if(!messages) return;

    cJSON *msg_item;
    int new_messages = 0;
    cJSON_ArrayForEach(msg_item, messages) {
        cJSON *uid = cJSON_GetObjectItem(msg_item, "uid");
        cJSON *sender_pk = cJSON_GetObjectItem(msg_item, "sender_pk");
        cJSON *content = cJSON_GetObjectItem(msg_item, "content");
        cJSON *vc_str_item = cJSON_GetObjectItem(msg_item, "vector_clock");

        if (cJSON_IsString(uid) && cJSON_IsString(sender_pk) && cJSON_IsString(content) && cJSON_IsString(vc_str_item)) {
            char my_pk_hex_local[PK_HEX_LEN+1];
            sodium_bin2hex(my_pk_hex_local, sizeof(my_pk_hex_local), my_pk, sizeof(my_pk));
            const char* chat_id = strcmp(sender_pk->valuestring, my_pk_hex_local) == 0 ? chat_target_pk_hex : sender_pk->valuestring;
            db_save_message(uid->valuestring, chat_id, sender_pk->valuestring, content->valuestring, vc_str_item->valuestring);
            cJSON* remote_clock = cJSON_Parse(vc_str_item->valuestring);
            if (remote_clock) {
                cJSON* local_clock = db_get_vector_clock(chat_id);
                vc_merge(local_clock, remote_clock);
                db_save_vector_clock(chat_id, local_clock);
                cJSON_Delete(local_clock);
                cJSON_Delete(remote_clock);
            }
            new_messages++;
        }
    }
    if (new_messages > 0) {
        log_msg("[同步] 收到 %d 条历史消息。", new_messages);
    }
}

// --- 新增的设置接口实现 ---
const char* get_my_public_key_hex() {
    return my_pk_hex;
}

int get_my_p2p_port() {
    return my_p2p_port;
}

int get_online_peer_count() {
    int count = 0;
    pthread_mutex_lock(&peers_mutex);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i]) {
            count++;
        }
    }
    pthread_mutex_unlock(&peers_mutex);
    return count;
}
