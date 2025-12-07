# ZeroLink - P2P 端到端加密聊天系统

`ZeroLink` 是一个基于C语言开发的、端到端加密的P2P（点对点）聊天软件原型。本项目旨在探索和实现一个无中心消息服务器、通信内容保密、用户身份自托管的现代化安全通信工具。

---

## 📘 技术设计大纲 (V2)

这份大纲是指导项目开发的核心技术文档，旨在实现一个健壮、安全且完全去中心化的P2P聊天系统。

### 1. 目标与基本架构

- **核心目标**: 实现完全去中心化的私聊与群聊功能。
- **引导服务器 (Bootstrapping Server)**:
    - 仅负责：用户ID与公钥的映射、在线节点地址的查询、NAT穿透的信令交换。
    - **绝不**转发任何聊天消息内容。
- **加密与存储**:
    - 所有消息均实现**端到端加密 (E2EE)**。
    - 所有聊天记录在每个设备本地均存储为**哈希链接的日志 (Append-only Log)**，类似于区块链结构，以防止篡改。

---

### 2. 核心功能模块结构

#### 2.1 引导服务器 (Bootstrapping)

- **功能**:
    1.  注册与查询：管理用户ID、公钥及其当前的公网地址（IP:Port）。
    2.  信令交换：在P2P直连建立前，作为中间人协调 **NAT Hole Punching** 过程。
- **接口 (REST + UDP)**:
    - `/register`: 注册或更新节点信息。
    - `/lookup`: 查询目标用户/节点的地址信息。
    - `/signal`: 用于NAT穿透的信令转发。

#### 2.2 P2P 连接 (直连优先，带回退机制)

- **连接状态机**:
    ```
    TryHolePunch()
    if OK: return DIRECT_CONNECTION
    TryPeerRelay()
    if OK: return PEER_RELAY
    UseServerRelay()
    return SERVER_RELAY
    ```

- **2.2.1 直连 (UDP Hole Punching)**
    - **首选方案**，通过引导服务器交换信令，尝试建立直接的UDP连接。
    - 成功后，在此基础上运行可靠UDP协议或QUIC。

- **2.2.2 二级中继 (Peer Relay)**
    - 当直连失败时，选择一个或多个与通信双方都已建立连接的在线节点作为中继。
    - 中继节点仅转发**加密数据包**，无法解密内容。

- **2.2.3 三级中继 (Server Relay)**
    - **备选方案**，使用官方提供的轻量级中继服务器（类似TURN）。
    - 服务器仅对加密流量进行透明转发，不存储、不记录。

---

### 3. 消息系统 (区块链式日志)

#### 3.1 区块格式 (`ChatBlock`)

```c
struct ChatBlock {
    uint64_t index;          // 区块索引
    unsigned char prev_hash[32]; // 前一区块的哈希
    unsigned char sender_pubkey[...]; // 发送者公钥
    unsigned char signature[...];     // 对区块头+密文的签名
    uint64_t timestamp;      // UTC时间戳
    unsigned char *ciphertext; // 加密的JSON Payload
    size_t ciphertext_len;
};
```
- **特性**:
    - **防篡改**: 通过哈希链确保历史记录的完整性。
    - **防伪造**: 通过数字签名验证发送者身份。
    - **保密性**: 内容使用强加密算法（如AES-GCM）加密。

#### 3.2 密钥系统

- **私聊**:
    - 初始密钥交换采用 **X3DH** 或简化版 Diffie-Hellman 协议。
    - 可选实现 **Double Ratchet** 算法，为每条消息提供前向保密和后向保密。
- **群聊**:
    - **群密钥**需要通过带外方式（out-of-band）安全分发。
    - 新成员加入时，必须重新生成并安全分发新的群密钥。
    - 可选实现群密钥的自动轮换逻辑。

---

### 4. 群聊消息同步机制 (Diff Sync)

- **模型**: 每个群成员都在本地保留一份完整的、相同的区块链式聊天记录。
- **同步协议**:
    1.  一个节点上线后，向其他在线节点广播自己的**最新区块索引**和**最近几个区块的哈希**。
    2.  其他节点对比后，将该节点缺失的区块发送给它。
    3.  接收节点在本地验证收到的区块（校验哈希链和签名）。
    4.  验证通过后，将区块追加到本地日志中。
- **伪代码**:
    ```
    RequestSync(my_latest_index, recent_hashes);
    ReceiveMissingBlocks(blocks[]);
    for (block in blocks) {
        VerifySignature(block);
        VerifyHashChain(block);
    }
    AppendBlocksToLocalDB(blocks);
    ```

---

### 5. 私聊离线消息机制 (Store-and-forward by Peers)

- **流程**:
    1.  A 向 B 发送消息。
    2.  A 将消息区块追加到自己的本地链。
    3.  如果 B 离线，A（或其他在线的共同好友）会缓存这条消息。
    4.  B 上线后，通过**同步协议 (Diff Sync)** 从 A 或其他节点拉取缺失的消息。
    5.  B 验证并写入本地链。
- **特点**: 无需中心服务器存储任何离线消息。

---

### 6. 网络协议格式

- **包类型 (PacketType)**:
    - `HELLO`: 连接建立后的初始握手。
    - `PING` / `PONG`: 保持连接和延迟检测。
    - `HOLE_PUNCH`: NAT穿透信令。
    - `MESSAGE_BLOCK`: 承载一个或多个 `ChatBlock`。
    - `SYNC_REQUEST`: 请求同步消息。
    - `SYNC_RESPONSE`: 回应同步请求，包含缺失的区块。
    - `RELAY_WRAPPED_PACKET`: 经由中继转发的包。
- **加密**:
    - **信令**: 明文传输。
    - **消息**: 密文传输 (使用基于ECDH派生的对称密钥，如AES-GCM)。

---

### 7. 本地存储

- **数据库选型**: LevelDB, SQLite, 或 RocksDB。
- **数据结构**:
    - 主要数据以 `ChatBlock` 的形式序列化后存储。
    - 按 `index` 建立主索引，方便快速查找和范围查询。
- **存储路径**:
    - 推荐: `/data/<user_id>/chatlogs/<chat_id>.db`

---

### 8. 模块划分 (代码组织结构)

```
/core
    /crypto/      # 加解密、签名、哈希
    /storage/     # 数据库接口与实现
    /net/         # 底层网络socket
    /p2p/         # P2P连接管理、NAT穿透
    /relay/       # 中继逻辑
    /protocol/    # 网络包序列化/反序列化
    /models/      #核心数据结构 (ChatBlock)
/client
    /ui/          # 用户界面
    /logic/       # 客户端业务逻辑
    /settings/    # 配置管理
/server
    /bootstrap/   # 引导服务器实现
    /relay/       # 官方中继服务器实现
```

---

### 9. 优先开发顺序 (v0.1.1)

- ✅ **定义 `ChatBlock` 数据结构与数据库存储层接口。**
- 🔄 **实现端到端加密模块 (`/core/crypto`)**: _进行中。加密逻辑已在业务代码中实现，但尚未完全抽象成独立模块。_
- 🔄 **实现消息链的本地存储 (`/core/storage`)**: _进行中。当前使用SQLite存储消息，但尚未实现基于哈希链的区块验证。_
- 🔄 **实现引导服务器 (`/server/bootstrap`) 和客户端的 `Hole Punching` 逻辑**: _进行中。引导服务器已模块化，但NAT穿透逻辑未实现。_
- ✅ **实现P2P直连通信**: _已完成。客户端之间可建立TCP连接并交换加密消息。_
- ⬜ **实现群聊的广播和消息同步协议**: _未开始。_
- ✅ **实现私聊的离线消息机制**: _已完成。基于向量时钟的同步协议，客户端上线后可自动同步私聊消息。_
- ⬜ **实现 Peer Relay 和 Server Relay 作为回退方案**: _未开始。_

---

### 10. 可选高级功能 (未来展望)

- 多设备同步（可能全平台哦！）
- 双棘轮算法 (Signal Protocol)
- 基于洋葱路由的匿名化(可能吧)
- P2P文件传输
