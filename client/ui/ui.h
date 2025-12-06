#ifndef ZEROLINK_UI_H
#define ZEROLINK_UI_H

#include <ncurses.h>
#include "../../core/models/friend.h" // For PK_HEX_LEN

// --- UI 状态机 ---
typedef enum {
    UI_STATE_FRIENDS_LIST,
    UI_STATE_CHATTING,
    UI_STATE_ADD_FRIEND_PK,
    UI_STATE_ADD_FRIEND_NAME,
    UI_STATE_CONFIRM_DELETE,
    UI_STATE_EXITING
} UIState;

// --- 全局UI状态变量 ---
extern UIState current_ui_state;
extern int selected_friend_index;
extern char chat_target_name[32];
extern char chat_target_pk_hex[PK_HEX_LEN + 1];

// --- 函数原型 ---

void init_ui();
void destroy_ui();
void redraw_ui();
void add_line_to_messages(const char* msg);
void handle_input_and_events();

/**
 * @brief 从日志队列中取出消息并更新到UI上。
 * 这个函数应该在主UI循环中被调用。
 */
void update_logs_from_queue();

/**
 * @brief (线程安全) 将一条日志消息放入队列，等待UI线程处理。
 * @param msg 要排队的日志消息。
 */
void queue_log_message(const char *msg);


#endif //ZEROLINK_UI_H
