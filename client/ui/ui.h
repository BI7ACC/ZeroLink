#ifndef ZEROLINK_UI_H
#define ZEROLINK_UI_H

#include <ncurses.h>
#include "../../core/models/friend.h"

// --- UI 状态机 ---
typedef enum {
    UI_STATE_MAIN,          // 主界面，带标签页
    UI_STATE_CHATTING,      // 全屏聊天界面
    UI_STATE_ADD_FRIEND_PK,
    UI_STATE_ADD_FRIEND_NAME,
    UI_STATE_EXITING
} UIState;

// --- 全局UI状态变量 ---
extern UIState current_ui_state;
extern int main_tab_index;      // 主界面当前选中的标签 (0-3)
extern int friend_list_index; // 主界面好友列表中选中的好友
extern char chat_target_name[32];
extern char chat_target_pk_hex[PK_HEX_LEN + 1];

// --- 函数原型 ---
void init_ui();
void destroy_ui();
void redraw_ui();
void handle_input_and_events();

/**
 * @brief 从日志队列中取出消息并更新到UI上。
 */
void update_logs_from_queue();

/**
 * @brief (线程安全) 将一条日志消息放入队列，等待UI线程处理。
 */
void queue_log_message(const char *msg);

#endif //ZEROLINK_UI_H
