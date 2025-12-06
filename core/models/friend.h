#ifndef ZEROLINK_FRIEND_H
#define ZEROLINK_FRIEND_H

#include <sodium.h>

// 统一的公钥十六进制长度定义
#define PK_HEX_LEN (crypto_box_PUBLICKEYBYTES * 2)

/**
 * @struct friend_t
 * @brief 代表一个好友的信息。
 */
typedef struct {
    char name[32];
    char pk_hex[PK_HEX_LEN + 1];
} friend_t;

#endif //ZEROLINK_FRIEND_H
