#include "ui.h"
#include "../logic/client_logic.h"
#include "../../core/models/friend.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <locale.h>

UIState current_ui_state = UI_STATE_FRIENDS_LIST;
int selected_friend_index = 0;
char chat_target_name[32];
char chat_target_pk_hex[PK_HEX_LEN + 1];

static WINDOW *msg_win, *input_win, *msg_border_win, *input_border_win;
static WINDOW *menu_win; // 新增：专门用于主页菜单的窗口
static volatile sig_atomic_t ui_needs_resize = 0;

#define MAX_LOG_MESSAGES 100
static char *log_queue[MAX_LOG_MESSAGES];
static int log_queue_head = 0, log_queue_tail = 0;
static pthread_mutex_t log_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static void draw_main_layout();
static void draw_chat_view();
static void handle_winch(int sig);

void init_ui() {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(1);
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_YELLOW, -1);
    signal(SIGWINCH, handle_winch);
    redraw_ui();
}

void destroy_ui() {
    delwin(msg_win);
    delwin(input_win);
    delwin(msg_border_win);
    delwin(input_border_win);
    if(menu_win) delwin(menu_win);
    endwin();
}

void add_line_to_messages(const char* msg) {
    if (msg_win) {
        wprintw(msg_win, "%s\n", msg);
        wrefresh(msg_win);
        wrefresh(input_win);
    }
}

static void draw_main_layout() {
    int height, width;
    getmaxyx(stdscr, height, width);
    
    // 绘制好友列表和菜单
    int menu_height = 10;
    if (menu_win) delwin(menu_win);
    menu_win = newwin(menu_height, width, height - 3 - menu_height, 0);
    box(menu_win, 0, 0);
    wattron(menu_win, COLOR_PAIR(1));
    mvwprintw(menu_win, 0, 2, " 菜单 ");
    wattroff(menu_win, COLOR_PAIR(1));

    int friend_count = get_friend_count();
    friend_t** friends = get_friends();

    mvwprintw(menu_win, 1, 2, "好友列表:");
    for (int i = 0; i < friend_count; i++) {
        mvwprintw(menu_win, 2 + i, 4, "%d. %s", i + 1, friends[i]->name);
    }

    int menu_start_line = 2 + friend_count + 1;
    mvwprintw(menu_win, menu_start_line, 2, "操作:");
    mvwprintw(menu_win, menu_start_line + 1, 4, "[数字] - 聊天 | [a] - 添加 | [q] - 退出");
    
    wrefresh(menu_win);
}

static void draw_chat_view() {
    werase(msg_win);
    db_load_history(chat_target_pk_hex);
}

void redraw_ui() {
    int height, width;
    getmaxyx(stdscr, height, width);

    if (msg_border_win) delwin(msg_border_win);
    if (input_border_win) delwin(input_border_win);
    if (msg_win) delwin(msg_win);
    if (input_win) delwin(input_win);
    if (menu_win) { delwin(menu_win); menu_win = NULL; }

    clear();

    int msg_win_height;
    if (current_ui_state == UI_STATE_FRIENDS_LIST) {
        msg_win_height = height - 3 - 10; // 主页的消息窗口高度
    } else {
        msg_win_height = height - 3; // 其他页面的消息窗口高度
    }

    msg_border_win = newwin(msg_win_height, width, 0, 0);
    input_border_win = newwin(3, width, height - 3, 0);
    box(msg_border_win, 0, 0);
    box(input_border_win, 0, 0);
    
    msg_win = newwin(msg_win_height - 2, width - 2, 1, 1);
    input_win = newwin(1, width - 2, height - 2, 1);
    
    scrollok(msg_win, TRUE);
    keypad(input_win, TRUE);
    
    wattron(input_border_win, COLOR_PAIR(1));
    mvwprintw(input_border_win, 0, 2, " 输入 ");
    wattroff(input_border_win, COLOR_PAIR(1));

    const char* title = "ZeroLink";
    wattron(msg_border_win, COLOR_PAIR(1));
    mvwprintw(msg_border_win, 0, width - strlen(title) - 4, " %s ", title);

    if (current_ui_state == UI_STATE_FRIENDS_LIST) {
        mvwprintw(msg_border_win, 0, 2, " 系统日志 ");
        draw_main_layout();
    } else if (current_ui_state == UI_STATE_CHATTING) {
        mvwprintw(msg_border_win, 0, 2, " 正在与 %s 聊天 (输入 /back 返回) ", chat_target_name);
        draw_chat_view();
    } else if (current_ui_state == UI_STATE_ADD_FRIEND_PK) {
        mvwprintw(msg_border_win, 0, 2, " 添加好友 - 请输入公钥 ");
        werase(msg_win);
    } else if (current_ui_state == UI_STATE_ADD_FRIEND_NAME) {
        mvwprintw(msg_border_win, 0, 2, " 添加好友 - 请输入昵称 ");
        werase(msg_win);
    }
    wattroff(msg_border_win, COLOR_PAIR(1));

    refresh();
    wrefresh(msg_border_win);
    wrefresh(input_border_win);
    wrefresh(msg_win);
    wrefresh(input_win);
    if(menu_win) wrefresh(menu_win);
    ui_needs_resize = 0;
}

static void handle_winch(int sig) {
    ui_needs_resize = 1;
}

void handle_input_and_events() {
    static char input_buffer[4096] = {0};
    static int i = 0;
    static char add_friend_pk_buf[PK_HEX_LEN + 1];

    if (ui_needs_resize) {
        redraw_ui();
    }

    int ch;
    wtimeout(input_win, 100);
    ch = wgetch(input_win);

    if (ch != ERR) {
        if (ch == '\n') {
            if (i > 0) {
                input_buffer[i] = '\0';
                
                UIState previous_state = current_ui_state;

                if (current_ui_state == UI_STATE_FRIENDS_LIST) {
                    int friend_idx = atoi(input_buffer) - 1;
                    if (friend_idx >= 0 && friend_idx < get_friend_count()) {
                        friend_t** friends = get_friends();
                        strcpy(chat_target_name, friends[friend_idx]->name);
                        strcpy(chat_target_pk_hex, friends[friend_idx]->pk_hex);
                        current_ui_state = UI_STATE_CHATTING;
                        request_chat_sync(chat_target_pk_hex);
                    } else if (strcmp(input_buffer, "a") == 0) {
                        current_ui_state = UI_STATE_ADD_FRIEND_PK;
                    } else if (strcmp(input_buffer, "q") == 0) {
                        current_ui_state = UI_STATE_EXITING;
                    }
                } else if (current_ui_state == UI_STATE_CHATTING) {
                    if (strcmp(input_buffer, "/back") == 0) {
                        current_ui_state = UI_STATE_FRIENDS_LIST;
                    } else {
                        send_chat_message(chat_target_name, input_buffer);
                    }
                } else if (current_ui_state == UI_STATE_ADD_FRIEND_PK) {
                    if (strlen(input_buffer) != PK_HEX_LEN) {
                        log_msg("[系统] 错误: 公钥长度不正确。");
                    } else {
                        strcpy(add_friend_pk_buf, input_buffer);
                        current_ui_state = UI_STATE_ADD_FRIEND_NAME;
                    }
                } else if (current_ui_state == UI_STATE_ADD_FRIEND_NAME) {
                    add_new_friend(add_friend_pk_buf, input_buffer);
                    current_ui_state = UI_STATE_FRIENDS_LIST;
                }
                
                i = 0;
                memset(input_buffer, 0, sizeof(input_buffer));
                
                if (previous_state != current_ui_state) {
                    redraw_ui();
                }
                wclear(input_win);
                wrefresh(input_win);
            }
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            if (i > 0) {
                i--;
                mvwdelch(input_win, 0, i);
            }
        } else if (i < sizeof(input_buffer) - 1) {
            input_buffer[i++] = ch;
            waddch(input_win, ch);
        }
    }
}

void queue_log_message(const char *msg) {
    pthread_mutex_lock(&log_queue_mutex);
    if ((log_queue_head + 1) % MAX_LOG_MESSAGES != log_queue_tail) {
        log_queue[log_queue_head] = strdup(msg);
        log_queue_head = (log_queue_head + 1) % MAX_LOG_MESSAGES;
    }
    pthread_mutex_unlock(&log_queue_mutex);
}

void update_logs_from_queue() {
    pthread_mutex_lock(&log_queue_mutex);
    if (log_queue_tail != log_queue_head) {
        while (log_queue_tail != log_queue_head) {
            add_line_to_messages(log_queue[log_queue_tail]);
            free(log_queue[log_queue_tail]);
            log_queue_tail = (log_queue_tail + 1) % MAX_LOG_MESSAGES;
        }
    }
    pthread_mutex_unlock(&log_queue_mutex);
}
