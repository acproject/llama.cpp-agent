## 测试用例
### 1. 基本连接测试

```sh
MC> PING
MC> SET mykey "Hello World"
MC> GET mykey
MC> EXISTS mykey
MC> DEL mykey
MC> GET mykey
```
### 2.数据库选择测试
```sh
MC> SELECT 1
MC> SET db1key "Database 1 Value"
MC> GET db1key
MC> SELECT 0
MC> GET db1key
MC> SET db0key "Database 0 Value"
MC> SELECT 1
MC> GET db0key
```
### 3.事务测试
```sh
MC> MULTI
MC> SET tx_key1 "Transaction Value 1"
MC> SET tx_key2 "Transaction Value 2"
MC> EXEC
MC> GET tx_key1
MC> GET tx_key2
```

### 4.监视键测试
```sh
MC> SET watch_key "Initial Value"
MC> MULTI
MC> WATCH watch_key
MC> SET watch_key "New Value"
MC> EXEC
MC> GET watch_key
```

### 5.过期时间测试
```sh
MC> SET expire_key "This will expire"
MC> PEXPIRE expire_key 50000
MC> PTTL expire_key
MC> GET expire_key
# 等待5秒后
MC> GET expire_key
```

### 6.键空间操作测试
```sh
MC> FLUSHDB
MC> SET key1 "value1"
MC> SET key2 "value2"
MC> SET key3 "value3"
MC> KEYS *                  
MC> SCAN key*
```

### 7.数值操作测试
```sh
MC> SET counter "10"
MC> INCR counter
MC> GET counter
MC> SETNX numbers 1.0 2.0 3.0 4.0     
MC> GETNX numbers                     
```

### 8.持久化测试
```sh
MC> SET persist_key "This will be saved"
MC> SAVE
# 重启服务器后
MC> GET persist_key
```

### 9.压力测试脚本
```bash
#!/bin/bash

# 并发客户端数量
NUM_CLIENTS=5

# 每个客户端执行的命令数
NUM_COMMANDS=100

for ((i=1; i<=$NUM_CLIENTS; i++)); do
  (
    echo "客户端 $i 开始测试..."
    for ((j=1; j<=$NUM_COMMANDS; j++)); do
      echo "SET key_${i}_${j} value_${i}_${j}" | ./mini_cache_cli -h 127.0.0.1 -p 6379 > /dev/null
      echo "GET key_${i}_${j}" | ./mini_cache_cli -h 127.0.0.1 -p 6379 > /dev/null
    done
    echo "客户端 $i 完成测试"
  ) &
done

wait
echo "所有客户端测试完成"
```

### 10.图谱与证据检索（结构化即推理）

```sh
# 写入 chunk 原文与向量（__chunk:<id> <-> __emb:<id>）
MC> OBJSET __chunk:doc1:0 text/plain "Redis 是一个内存数据库"
MC> SETNX __emb:demo_d3_cosine:doc1:0 0.10 0.20 0.30
MC> TAGADD __chunk:doc1:0 redis kv
MC> METASET __chunk:doc1:0 source wiki

# 建图：topic -> chunk
MC> METASET topic:redis type topic
MC> GRAPH.ADDEDGE topic:redis HAS_CHUNK __chunk:doc1:0
MC> GRAPH.EDGEPROP.SET topic:redis HAS_CHUNK __chunk:doc1:0 confidence 0.92

# 结构化邻居（固定槽位）：[rel, to, edge_props, edge_meta, from_meta, to_meta]
MC> GRAPH.NEIGHBORSX2 topic:redis HAS_CHUNK 10 EDGE_METAKEYS 1 confidence FROM_METAKEYS 1 type TO_METAKEYS 1 source

# 枚举边（固定槽位）：[from, rel, to, edge_props, edge_meta, from_meta, to_meta]
MC> GRAPH.EDGE.LIST2 topic:redis HAS_CHUNK 10 EDGE_METAKEYS 1 confidence FROM_METAKEYS 1 type TO_METAKEYS 1 source

# 证据检索：只在 topic:redis 可达的 chunk/emb 中检索
MC> EVIDENCE.SEARCHF 5 cosine 3 0.11 0.19 0.29 GRAPHFROM topic:redis GRAPHREL HAS_CHUNK GRAPHDEPTH 1 TAG redis META source wiki
```

### 11.multilingual-e5-large-instruct（llama.cpp embedding）端到端示例

E5 系列的典型范式是区分 query 与 passage 前缀：

- passage（文档/段落）：`passage: <chunk_text>`
- query（用户问题）：`query: <question>`

本示例推荐固定空间名（避免后续换模型混用）：

- embedding key：`__emb:e5-multi-large-instruct_d1024_cosine:<id>`
- chunk key：`__chunk:<id>`
- dim：`1024`
- metric：`cosine`

```sh
# 1) 写入 chunk 原文（供 EVIDENCE.* 回填证据）
MC> OBJSET __chunk:doc2:0 text/plain "Redis 支持多种数据结构，例如 string/list/set/zset/hash。"
MC> TAGADD __chunk:doc2:0 redis
MC> METASET __chunk:doc2:0 source handbook

# 2) 外部用 llama.cpp 生成 passage embedding（示例程序名可能是 embedding / llama-embedding）
#    ./embedding -m multilingual-e5-large-instruct-*.gguf -p "passage: Redis 支持多种数据结构，例如 string/list/set/zset/hash。"
#
#    取输出的 1024 维向量，写入：
MC> SETNX __emb:e5-multi-large-instruct_d1024_cosine:doc2:0 <f1> <f2> ... <f1024>

# 3)（可选）建图，用于 EVIDENCE.SEARCHF 的候选集裁剪
MC> METASET topic:redis type topic
MC> GRAPH.ADDEDGE topic:redis HAS_CHUNK __chunk:doc2:0

# 4) 外部用 llama.cpp 生成 query embedding
#    ./embedding -m multilingual-e5-large-instruct-*.gguf -p "query: Redis 有哪些数据结构？"
#
# 5) 用 query 向量做检索，并按图谱 + tag/meta 约束候选集
MC> EVIDENCE.SEARCHF 5 cosine 1024 <q1> <q2> ... <q1024> GRAPHFROM topic:redis GRAPHREL HAS_CHUNK GRAPHDEPTH 1 TAG redis META source handbook
```
