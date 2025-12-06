#ifndef ZEROLINK_CHAT_BLOCK_H
#define ZEROLINK_CHAT_BLOCK_H

#include <stdint.h>
#include <sodium.h>

// 根据技术大纲定义哈希和签名的字节长度
#define HASH_BYTES 32 // 使用 SHA-256
#define SIGNATURE_BYTES crypto_sign_BYTES

/**
 * @struct ChatBlock
 * @brief 代表聊天记录中一个独立的、可验证的区块。
 *
 * 这是本地存储的核心数据单元，形成一个哈希链（类似区块链）。
 * 该结构体主要用于内存中的表示，其中 ciphertext 是动态分配的。
 */
typedef struct {
    /// @brief 区块在链中的索引，从 0 开始。
    uint64_t index;

    /// @brief 前一个区块的哈希值，确保链的连续性。
    unsigned char prev_hash[HASH_BYTES];

    /// @brief 发送者的公钥。
    unsigned char sender_pubkey[crypto_box_PUBLICKEYBYTES];

    /// @brief 发送者对 (index + prev_hash + sender_pubkey + timestamp + ciphertext) 的签名。
    unsigned char signature[SIGNATURE_BYTES];

    /// @brief 区块创建时的UTC时间戳。
    uint64_t timestamp;

    /// @brief 加密后的消息内容 (JSON payload)。
    /// 在序列化到磁盘时，这是一个可变长度的字段。
    unsigned char *ciphertext;
    size_t ciphertext_len;

} ChatBlock;

#endif //ZEROLINK_CHAT_BLOCK_H
