#include "ui.h"
#include "../logic/client_logic.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <locale.h>

// --- 全局UI状态变量定义 ---
UIState current_ui_state = UI_STATE_MAIN;
int main_tab_index = 0;
int friend_list_index = 0;
char chat_target_name[32];
char chat_target_pk_hex[PK_HEX_LEN + 1];

// --- 内部UI组件和状态 ---
static WINDOW *log_win, *content_win, *input_win;
static WINDOW *log_border, *content_border, *input_border;
static volatile sig_atomic_t ui_needs_resize = 0;
const char* TABS[] = {"好友", "添加好友", "设置", "退出"};
const int NUM_TABS = sizeof(TABS)/sizeof(TABS[0]);

// --- 日志队列 ---
#define MAX_LOG_MESSAGES 100
static char *log_queue[MAX_LOG_MESSAGES];
static int log_queue_head = 0, log_queue_tail = 0;
static pthread_mutex_t log_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- 内部函数原型 ---
static void draw_main_view();
static void draw_chat_view();
static void handle_winch(int sig);
static void draw_tabs();
static void add_line_to_window(WINDOW *win, const char* msg);
static void delete_windows();

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
    init_pair(4, COLOR_WHITE, COLOR_CYAN); // 高亮标签
    signal(SIGWINCH, handle_winch);
    keypad(stdscr, TRUE);
    redraw_ui();
}

void destroy_ui() {
    endwin();
}

static void delete_windows() {
    if(log_win) { delwin(log_win); log_win = NULL; }
    if(content_win) { delwin(content_win); content_win = NULL; }
    if(input_win) { delwin(input_win); input_win = NULL; }
    if(log_border) { delwin(log_border); log_border = NULL; }
    if(content_border) { delwin(content_border); content_border = NULL; }
    if(input_border) { delwin(input_border); input_border = NULL; }
}


static void add_line_to_window(WINDOW *win, const char* msg) {
    if (win) {
        wprintw(win, "%s\n", msg);
        wrefresh(win);
    }
}

static void draw_tabs() {
    int x_offset = 1;
    wattron(content_border, COLOR_PAIR(1));
    
    for (int i = 0; i < NUM_TABS; i++) {
        mvwaddstr(content_border, 0, x_offset, "─");
        x_offset++;

        if (i == main_tab_index) {
            wattron(content_border, COLOR_PAIR(4));
            mvwprintw(content_border, 0, x_offset, " %s ", TABS[i]);
            wattroff(content_border, COLOR_PAIR(4));
        } else {
            mvwprintw(content_border, 0, x_offset, " %s ", TABS[i]);
        }
        x_offset += strlen(TABS[i]) + 2;
        mvwaddstr(content_border, 0, x_offset, "─");
        x_offset++;
    }
    wattroff(content_border, COLOR_PAIR(1));
    wrefresh(content_border);
}

static void draw_main_view() {
    werase(content_win);
    
    if (main_tab_index == 0) { // 好友
        int friend_count = get_friend_count();
        friend_t** friends = get_friends();
        for (int i = 0; i < friend_count; i++) {
            if (i == friend_list_index) wattron(content_win, A_REVERSE);
            mvwprintw(content_win, 1 + i, 2, "%s", friends[i]->name);
            if (i == friend_list_index) wattroff(content_win, A_REVERSE);
        }
    } else if (main_tab_index == 1) { // 添加好友
        mvwprintw(content_win, 1, 2, "按回车键进入添加好友流程。");
    } else if (main_tab_index == 2) { // 设置
        mvwprintw(content_win, 1, 2, "本机公钥 (ID): %s", get_my_public_key_hex());
        mvwprintw(content_win, 2, 2, "P2P 监听端口: %d", get_my_p2p_port());
        mvwprintw(content_win, 3, 2, "在线好友数: %d / %d", get_online_peer_count(), get_friend_count());
    } else if (main_tab_index == 3) { // 退出
        mvwprintw(content_win, 1, 2, "按回车键退出程序。");
    }
    wrefresh(content_win);
}

static void draw_chat_view() {
    werase(content_win);
    db_load_history(chat_target_pk_hex);
}

void redraw_ui() {
    int height, width;
    getmaxyx(stdscr, height, width);

    delete_windows();
    clear();
    refresh();

    if (current_ui_state == UI_STATE_MAIN) {
        log_border = newwin(4, width, 0, 0);
        content_border = newwin(height - 4, width, 4, 0);
        log_win = newwin(2, width - 2, 1, 1);
        content_win = newwin(height - 4 - 2, width - 2, 5, 1);
        
        box(log_border, 0, 0);
        box(content_border, 0, 0);
        wattron(log_border, COLOR_PAIR(1));
        mvwprintw(log_border, 0, 2, " 系统日志 ");
        wattroff(log_border, COLOR_PAIR(1));
        
        scrollok(log_win, TRUE);
        scrollok(content_win, TRUE);
        
        draw_tabs();
        draw_main_view();

    } else {
        content_border = newwin(height - 3, width, 0, 0);
        input_border = newwin(3, width, height - 3, 0);
        content_win = newwin(height - 3 - 2, width - 2, 1, 1);
        input_win = newwin(1, width - 2, height - 2, 1);

        box(content_border, 0, 0);
        box(input_border, 0, 0);
        scrollok(content_win, TRUE);
        keypad(input_win, TRUE);

        wattron(input_border, COLOR_PAIR(1));
        mvwprintw(input_border, 0, 2, " 输入 ");
        wattroff(input_border, COLOR_PAIR(1));
        
        if (current_ui_state == UI_STATE_CHATTING) {
            wattron(content_border, COLOR_PAIR(2));
            mvwprintw(content_border, 0, 2, " 正在与 %s 聊天 ", chat_target_name);
            wattroff(content_border, COLOR_PAIR(2));
            draw_chat_view();
        } else if (current_ui_state == UI_STATE_ADD_FRIEND_PK) {
            wattron(content_border, COLOR_PAIR(3));
            mvwprintw(content_border, 0, 2, " 添加好友 - 请输入公钥 ");
            wattroff(content_border, COLOR_PAIR(3));
        } else if (current_ui_state == UI_STATE_ADD_FRIEND_NAME) {
            wattron(content_border, COLOR_PAIR(3));
            mvwprintw(content_border, 0, 2, " 添加好友 - 请输入昵称 ");
            wattroff(content_border, COLOR_PAIR(3));
        }
        wclear(input_win);
    }

    if(log_border) wrefresh(log_border);
    if(content_border) wrefresh(content_border);
    if(input_border) wrefresh(input_border);
    if(log_win) wrefresh(log_win);
    if(content_win) wrefresh(content_win);
    if(input_win) wrefresh(input_win);
    
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
    if (current_ui_state == UI_STATE_MAIN) {
        wtimeout(stdscr, 100);
        ch = getch();
    } else {
        wtimeout(input_win, 100);
        ch = wgetch(input_win);
    }

    if (ch == ERR) return;

    if (current_ui_state == UI_STATE_MAIN) {
        switch(ch) {
            case KEY_LEFT:
                if (main_tab_index > 0) { main_tab_index--; draw_tabs(); draw_main_view(); }
                break;
            case KEY_RIGHT:
                if (main_tab_index < NUM_TABS - 1) { main_tab_index++; draw_tabs(); draw_main_view(); }
                break;
            case KEY_UP:
                if (main_tab_index == 0 && friend_list_index > 0) { friend_list_index--; draw_main_view(); }
                break;
            case KEY_DOWN:
                if (main_tab_index == 0 && friend_list_index < get_friend_count() - 1) { friend_list_index++; draw_main_view(); }
                break;
            case '\n':
                if (main_tab_index == 0) {
                    if (get_friend_count() > 0) {
                        friend_t** friends = get_friends();
                        strcpy(chat_target_name, friends[friend_list_index]->name);
                        strcpy(chat_target_pk_hex, friends[friend_list_index]->pk_hex);
                        current_ui_state = UI_STATE_CHATTING;
                        request_chat_sync(chat_target_pk_hex);
                        redraw_ui();
                    }
                } else if (main_tab_index == 1) {
                    current_ui_state = UI_STATE_ADD_FRIEND_PK;
                    redraw_ui();
                } else if (main_tab_index == 3) {
                    current_ui_state = UI_STATE_EXITING;
                }
                break;
        }
    } else {
        if (ch == '\n') {
            if (i > 0) {
                input_buffer[i] = '\0';
                UIState previous_state = current_ui_state;

                if (current_ui_state == UI_STATE_CHATTING) {
                    if (input_buffer[0] == '/') {
                        if (strcmp(input_buffer, "/back") == 0) {
                            current_ui_state = UI_STATE_MAIN;
                        } else if (strcmp(input_buffer, "/help") == 0) {
                            log_msg("[指令] 可用指令: /back, /help");
                        } else {
                            log_msg("[指令] 未知指令: %s", input_buffer);
                        }
                    } else {
                        send_chat_message(chat_target_name, input_buffer);
                    }
                } else if (current_ui_state == UI_STATE_ADD_FRIEND_PK) {
                    if (strlen(input_buffer) != PK_HEX_LEN) log_msg("[系统] 错误: 公钥长度不正确。");
                    else {
                        strcpy(add_friend_pk_buf, input_buffer);
                        current_ui_state = UI_STATE_ADD_FRIEND_NAME;
                    }
                } else if (current_ui_state == UI_STATE_ADD_FRIEND_NAME) {
                    add_new_friend(add_friend_pk_buf, input_buffer);
                    current_ui_state = UI_STATE_MAIN;
                }
                
                i = 0;
                memset(input_buffer, 0, sizeof(input_buffer));
                
                if (previous_state != current_ui_state) redraw_ui();
                else {
                    wclear(input_win);
                    wrefresh(input_win);
                }
            }
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            if (i > 0) { i--; mvwdelch(input_win, 0, i); }
        } else if (i < sizeof(input_buffer) - 1 && ch >= 32 && ch <= 126) {
            if (i == 0) wclear(input_win);
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
            if (current_ui_state == UI_STATE_MAIN && log_win) {
                add_line_to_window(log_win, log_queue[log_queue_tail]);
            } else if (content_win) {
                add_line_to_window(content_win, log_queue[log_queue_tail]);
            }
            free(log_queue[log_queue_tail]);
            log_queue_tail = (log_queue_tail + 1) % MAX_LOG_MESSAGES;
        }
    }
    pthread_mutex_unlock(&log_queue_mutex);
}
