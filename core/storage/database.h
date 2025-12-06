#ifndef ZEROLINK_DATABASE_H
#define ZEROLINK_DATABASE_H

#include "../models/chat_block.h"

/**
 * @file database.h
 * @brief 数据库存储层抽象接口。
 *
 * 该接口定义了所有与本地持久化存储相关的操作，
 * 使得上层逻辑与具体的数据库实现（如 SQLite, LevelDB）解耦。
 */

// 定义一个不透明的数据库句柄类型
typedef struct DatabaseHandle DatabaseHandle;

/**
 * @brief 打开或创建一个指定聊天ID的数据库实例。
 * @param db_path 数据库文件或目录的路径。
 * @return 成功则返回一个非空的数据库句柄，失败则返回 NULL。
 */
DatabaseHandle* db_open(const char* db_path);

/**
 * @brief 关闭数据库连接并释放句柄。
 * @param handle 要关闭的数据库句柄。
 */
void db_close(DatabaseHandle* handle);

/**
 * @brief 将一个新的聊天区块追加到数据库中。
 *
 * 实现时必须保证操作的原子性。
 *
 * @param handle 数据库句柄。
 * @param block 要追加的区块。
 * @return 成功返回 0，失败返回非 0。
 */
int db_append_block(DatabaseHandle* handle, const ChatBlock* block);

/**
 * @brief 根据索引获取一个聊天区块。
 * @param handle 数据库句柄。
 * @param index 要获取的区块的索引。
 * @return 成功则返回一个指向 ChatBlock 的指针（需要调用者释放），找不到则返回 NULL。
 */
ChatBlock* db_get_block_by_index(DatabaseHandle* handle, uint64_t index);

/**
 * @brief 获取最新的一个聊天区块。
 * @param handle 数据库句柄。
 * @return 成功则返回一个指向最新 ChatBlock 的指针（需要调用者释放），数据库为空则返回 NULL。
 */
ChatBlock* db_get_latest_block(DatabaseHandle* handle);

/**
 * @brief 获取指定范围内的区块。
 * @param handle 数据库句柄。
 * @param start_index 起始索引（包含）。
 * @param end_index 结束索引（包含）。
 * @param blocks_out 函数将分配一个数组来存放区块指针，调用者需要释放该数组以及其中的每个区块。
 * @param count_out 返回获取到的区块数量。
 * @return 成功返回 0，失败返回非 0。
 */
int db_get_blocks_in_range(DatabaseHandle* handle, uint64_t start_index, uint64_t end_index, ChatBlock*** blocks_out, uint64_t* count_out);


#endif //ZEROLINK_DATABASE_H
