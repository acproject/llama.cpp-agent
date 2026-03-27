# llama.cpp 并行请求处理机制

## 概述

llama.cpp 通过 **Batch（批处理）** 和 **Sequence（序列）** 机制实现了多个用户并发请求同一个模型的能力。其核心思想是将多个用户的不同请求打包到一个批次中进行并行推理，充分利用 GPU/CPU 的并行计算能力。

## 核心架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         Server Layer                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │   Client 1   │  │   Client 2   │  │   Client N   │  ...      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘           │
│         │                 │                 │                    │
│         ▼                 ▼                 ▼                    │
│  ┌─────────────────────────────────────────────────────┐        │
│  │              Task Queue (任务队列)                   │        │
│  │   [Task1, Task2, Task3, ..., Deferred Tasks]        │        │
│  └─────────────────────────┬───────────────────────────┘        │
│                            │                                     │
│                            ▼                                     │
│  ┌─────────────────────────────────────────────────────┐        │
│  │              Slot Manager (槽位管理器)               │        │
│  │   ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐       │        │
│  │   │ Slot 0 │ │ Slot 1 │ │ Slot 2 │ │ Slot N │       │        │
│  │   │ seq=0  │ │ seq=1  │ │ seq=2  │ │ seq=N │       │        │
│  │   └────────┘ └────────┘ └────────┘ └────────┘       │        │
│  └─────────────────────────┬───────────────────────────┘        │
└────────────────────────────┼────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Inference Engine                            │
│  ┌─────────────────────────────────────────────────────┐        │
│  │                    llama_batch                       │        │
│  │   ┌──────────────────────────────────────────────┐  │        │
│  │   │ token[]: [t1, t2, t3, t4, t5, t6, ...]       │  │        │
│  │   │ pos[]:   [0,  1,  0,  1,  0,  1,  ...]       │  │        │
│  │   │ seq_id[]:[0,  0,  1,  1,  2,  2,  ...]       │  │        │
│  │   └──────────────────────────────────────────────┘  │        │
│  └─────────────────────────┬───────────────────────────┘        │
│                            │                                     │
│                            ▼                                     │
│  ┌─────────────────────────────────────────────────────┐        │
│  │              KV Cache (Memory)                       │        │
│  │   ┌──────────┐ ┌──────────┐ ┌──────────┐            │        │
│  │   │  Seq 0   │ │  Seq 1   │ │  Seq 2   │  ...       │        │
│  │   │ [K,V]    │ │ [K,V]    │ │ [K,V]    │            │        │
│  │   └──────────┘ └──────────┘ └──────────┘            │        │
│  └─────────────────────────────────────────────────────┘        │
└─────────────────────────────────────────────────────────────────┘
```

## 一、核心数据结构

### 1.1 llama_batch（批处理结构）

`llama_batch` 是 llama.cpp 并行处理的核心数据结构，它允许将多个序列的 token 打包到一起进行批量推理：

```c
typedef struct llama_batch {
    int32_t n_tokens;       // 批次中的 token 总数
    
    llama_token  * token;   // token ID 数组
    float        * embd;    // embedding 数组（可选，用于 embedding 输入）
    llama_pos    * pos;     // 每个 token 在其序列中的位置
    int32_t      * n_seq_id; // 每个 token 所属的序列数量
    llama_seq_id ** seq_id; // 每个 token 所属的序列 ID 数组
    int8_t       * logits;  // 是否输出该 token 的 logits
} llama_batch;
```

**关键点：**
- 一个 batch 可以包含来自**多个不同序列**的 token
- 每个 token 通过 `seq_id` 标识属于哪个序列
- `pos` 记录 token 在其**所属序列**中的位置（不是全局位置）

### 1.2 llama_seq_id（序列标识符）

```c
typedef int32_t llama_seq_id;
```

每个用户请求被分配一个唯一的序列 ID。同一个序列的 token 共享 KV Cache 状态，不同序列之间相互隔离。

**序列 ID 的作用：**
- 标识独立的对话/请求上下文
- 隔离不同用户的 KV Cache
- 支持序列级别的操作（删除、复制、保存等）

### 1.3 核心参数配置

```c
struct llama_context_params {
    uint32_t n_ctx;       // 上下文长度（KV Cache 大小）
    uint32_t n_batch;     // 逻辑批次大小上限
    uint32_t n_ubatch;    // 物理批次大小上限
    uint32_t n_seq_max;   // 最大并发序列数
    // ...
};
```

| 参数 | 说明 |
|------|------|
| `n_ctx` | 总上下文长度，所有序列共享 |
| `n_batch` | 单次 `llama_decode` 调用可处理的最大 token 数 |
| `n_seq_max` | 支持的最大并发序列数 |

## 二、并行处理机制

### 2.1 Batch 构建

在并行场景下，服务器会将多个用户的请求 token 合并到一个 batch 中：

```c
// 伪代码示例：构建包含多序列的 batch
llama_batch batch = llama_batch_init(n_ctx, 0, 1);

// 用户 1 的 token（序列 ID = 1）
for (int i = 0; i < tokens_user1.size(); i++) {
    common_batch_add(batch, tokens_user1[i], i, {1}, false);
}

// 用户 2 的 token（序列 ID = 2）
for (int i = 0; i < tokens_user2.size(); i++) {
    common_batch_add(batch, tokens_user2[i], i, {2}, false);
}

// 用户 3 的 token（序列 ID = 3）
for (int i = 0; i < tokens_user3.size(); i++) {
    common_batch_add(batch, tokens_user3[i], i, {3}, true); // 最后一个 token 需要 logits
}

// 执行批量推理
llama_decode(ctx, batch);
```

### 2.2 Continuous Batching（连续批处理）

llama.cpp 支持 **Continuous Batching**（也称 Iteration-level Scheduling），其工作流程：

```
┌─────────────────────────────────────────────────────────────┐
│                    迭代循环 (Generation Loop)                │
│                                                              │
│  1. 清空 batch                                               │
│     common_batch_clear(batch);                               │
│                                                              │
│  2. 收集所有活跃序列的新 token                                │
│     for each active_sequence:                                │
│         add_sampled_token_to_batch(batch, seq_id);           │
│                                                              │
│  3. 如果 batch 为空（所有序列结束）                           │
│     清理 KV Cache，准备接收新请求                             │
│                                                              │
│  4. 如果有新请求等待且 slot 可用                              │
│     添加新序列的 prompt token 到 batch                        │
│                                                              │
│  5. 批量推理                                                  │
│     llama_decode(ctx, batch);                                │
│                                                              │
│  6. 为每个活跃序列采样下一个 token                            │
│     for each active_sequence:                                │
│         token = sample_from_logits(batch, seq_id);           │
│         if (is_eos(token)):                                  │
│             mark_sequence_finished(seq_id);                  │
│                                                              │
│  7. 重复步骤 1-6                                             │
└─────────────────────────────────────────────────────────────┘
```

**关键特性：**
- 新请求可以在当前生成周期中加入
- 完成的序列可以立即释放资源
- 最大化 GPU 利用率

### 2.3 实际代码示例（来自 parallel.cpp）

```cpp
// 主处理循环
while (true) {
    common_batch_clear(batch);
    
    // 步骤 1: 收集所有活跃序列的下一个 token
    for (auto & client : clients) {
        if (client.seq_id == -1) continue;  // 跳过非活跃序列
        
        client.i_batch = batch.n_tokens;
        common_batch_add(batch, client.sampled, client.n_past++, 
                         {client.id + 1}, true);
        client.n_decoded += 1;
    }
    
    // 步骤 2: 如果 batch 为空，所有序列都已结束
    if (batch.n_tokens == 0) {
        // 清理 KV Cache
        for (int i = 1; i <= n_clients; ++i) {
            llama_memory_seq_rm(mem, i, -1, -1);
            llama_memory_seq_cp(mem, 0, i, -1, -1);  // 恢复系统 prompt
        }
    }
    
    // 步骤 3: 插入新序列（Continuous Batching）
    if (cont_batching || batch.n_tokens == 0) {
        for (auto & client : clients) {
            if (client.seq_id == -1 && g_seq_id < n_seq) {
                // 分配新序列 ID
                client.seq_id = g_seq_id++;
                // 添加 prompt token 到 batch
                for (auto token : client.prompt_tokens) {
                    common_batch_add(batch, token, client.n_past++, 
                                     {client.id + 1}, false);
                }
            }
        }
    }
    
    // 步骤 4: 分块处理（处理大 batch）
    for (int i = 0; i < batch.n_tokens; i += n_batch) {
        int n_tokens = std::min(n_batch, batch.n_tokens - i);
        llama_batch batch_view = {
            n_tokens,
            batch.token + i,
            nullptr,
            batch.pos + i,
            batch.n_seq_id + i,
            batch.seq_id + i,
            batch.logits + i,
        };
        
        llama_decode(ctx, batch_view);
        
        // 为每个序列采样
        for (auto & client : clients) {
            if (client.i_batch >= i && client.i_batch < i + n_tokens) {
                llama_token id = common_sampler_sample(
                    client.smpl, ctx, client.i_batch - i);
                client.sampled = id;
                // 检查是否结束...
            }
        }
    }
}
```

## 三、KV Cache / Memory 管理

### 3.1 Memory API

llama.cpp 提供了完整的 Memory 操作 API 用于管理序列的 KV Cache：

```c
// 获取上下文的 memory 句柄
llama_memory_t llama_get_memory(struct llama_context * ctx);

// 删除指定序列的 KV Cache
bool llama_memory_seq_rm(
    llama_memory_t mem,
    llama_seq_id seq_id,
    llama_pos p0,      // 起始位置，-1 表示从头开始
    llama_pos p1       // 结束位置，-1 表示到末尾
);

// 复制序列的 KV Cache（用于共享前缀）
void llama_memory_seq_cp(
    llama_memory_t mem,
    llama_seq_id seq_id_src,
    llama_seq_id seq_id_dst,
    llama_pos p0,
    llama_pos p1
);

// 保留指定序列的 KV Cache，删除其他序列
void llama_memory_seq_keep(
    llama_memory_t mem,
    llama_seq_id seq_id
);

// 调整序列的位置（用于 context shift）
void llama_memory_seq_add(
    llama_memory_t mem,
    llama_seq_id seq_id,
    llama_pos p0,
    llama_pos p1,
    llama_pos delta
);
```

### 3.2 共享系统 Prompt

当多个用户共享相同的系统 prompt 时，可以通过序列复制来避免重复计算：

```cpp
// 1. 首先将系统 prompt 编码到序列 0
for (int i = 0; i < n_tokens_system; ++i) {
    common_batch_add(batch, tokens_system[i], i, {0}, false);
}
llama_decode(ctx, batch);

// 2. 将序列 0 的 KV Cache 复制给所有用户序列
for (int i = 1; i <= n_clients; ++i) {
    llama_memory_seq_cp(mem, 0, i, -1, -1);
}

// 3. 现在每个用户序列都已包含系统 prompt 的 KV Cache
// 可以直接从 n_tokens_system 位置开始处理用户输入
```

### 3.3 Context Shift（上下文滑动）

当序列超出上下文长度时，需要进行 context shift：

```cpp
if (slot.prompt.n_tokens() + 1 >= slot.n_ctx) {
    // 方法 1: 清除部分 KV Cache
    llama_memory_seq_rm(mem, slot.seq_id, 0, n_discard);
    llama_memory_seq_add(mem, slot.seq_id, n_discard, -1, -n_discard);
    
    // 方法 2: 保留系统 prompt，清除对话历史
    llama_memory_seq_rm(mem, slot.seq_id, n_tokens_system, -1);
}
```

## 四、Server 架构实现

### 4.1 Slot 机制

Server 通过 **Slot（槽位）** 来管理并发请求：

```cpp
struct server_slot {
    int id;
    llama_context * ctx;
    llama_seq_id seq_id = -1;    // 当前分配的序列 ID
    
    // 状态机
    enum slot_state {
        SLOT_STATE_IDLE,              // 空闲
        SLOT_STATE_WAIT_OTHER,        // 等待父槽位处理
        SLOT_STATE_STARTED,           // 已启动
        SLOT_STATE_PROCESSING_PROMPT, // 处理 prompt 中
        SLOT_STATE_DONE_PROMPT,       // prompt 处理完成
        SLOT_STATE_GENERATING,        // 生成中
    };
    
    // 每个 slot 有独立的 sampler
    struct common_sampler * smpl;
    
    // 生成状态
    int32_t n_past = 0;
    int32_t n_decoded = 0;
    std::string response;
};
```

### 4.2 Task Queue（任务队列）

```cpp
struct server_queue {
    std::deque<server_task> queue_tasks;          // 主任务队列
    std::deque<server_task> queue_tasks_deferred; // 延迟队列（等待 slot）
    
    std::mutex mutex_tasks;
    std::condition_variable condition_tasks;
    
    // 主循环逻辑
    void start_loop(int64_t idle_sleep_ms = -1) {
        while (true) {
            // 1. 等待新任务
            // 2. 处理任务（分配到 slot）
            // 3. 检查多任务是否完成
            // 4. 更新所有 slots
        }
    }
};
```

### 4.3 请求处理流程

```
HTTP Request
     │
     ▼
┌─────────────────┐
│ 创建 server_task │
│ POST /completions│
└────────┬────────┘
         │
         ▼
┌─────────────────┐     无可用 slot
│  查找空闲 slot   │─────────────────┐
└────────┬────────┘                  │
         │ 有空闲                    ▼
         │                  ┌──────────────────┐
         ▼                  │ 加入延迟队列      │
┌─────────────────┐         │ queue_deferred   │
│ 分配 slot       │         └──────────────────┘
│ 设置 seq_id     │                  │
│ 初始化 sampler  │                  │ slot 释放后
└────────┬────────┘                  │ 自动加入主队列
         │                           │
         ▼                           │
┌─────────────────┐◀────────────────┘
│ update_slots()  │
│ 批量推理        │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 流式/非流式响应 │
│ server_response │
└─────────────────┘
```

## 五、性能优化策略

### 5.1 批处理优化

| 策略 | 说明 |
|------|------|
| **Dynamic Batching** | 动态调整批次大小，适应不同负载 |
| **Chunked Processing** | 大批次分块处理，避免内存溢出 |
| **logits 过滤** | 只计算需要输出的 token 的 logits |

### 5.2 KV Cache 优化

```cpp
// 配置 KV Cache 数据类型
struct llama_context_params {
    enum ggml_type type_k;  // K cache 数据类型（默认 f16）
    enum ggml_type type_v;  // V cache 数据类型（默认 f16）
    
    bool kv_unified;  // 统一 buffer 模式
    // 启用：适合共享前缀的场景
    // 禁用：适合独立序列的场景
};
```

### 5.3 Flash Attention

```cpp
enum llama_flash_attn_type {
    LLAMA_FLASH_ATTN_TYPE_AUTO = -1,     // 自动选择
    LLAMA_FLASH_ATTN_TYPE_DISABLED = 0,  // 禁用
    LLAMA_FLASH_ATTN_TYPE_ENABLED = 1,   // 启用
};
```

Flash Attention 可以显著提升多序列批处理的效率。

## 六、API 使用示例

### 6.1 基础并行推理

```cpp
#include "llama.h"
#include "common.h"

int main() {
    // 初始化
    llama_backend_init();
    
    // 加载模型
    llama_model * model = llama_model_load_from_file("model.gguf", 
                                                      llama_model_default_params());
    
    // 创建上下文，配置并行参数
    llama_context_params params = llama_context_default_params();
    params.n_ctx = 4096;
    params.n_batch = 512;
    params.n_seq_max = 8;  // 支持最多 8 个并发序列
    
    llama_context * ctx = llama_init_from_model(model, params);
    llama_memory_t mem = llama_get_memory(ctx);
    
    // 初始化 batch
    llama_batch batch = llama_batch_init(params.n_ctx, 0, 1);
    
    // 用户 1 的 prompt（序列 0）
    std::vector<llama_token> prompt1 = common_tokenize(ctx, "Hello", true);
    for (size_t i = 0; i < prompt1.size(); i++) {
        common_batch_add(batch, prompt1[i], i, {0}, i == prompt1.size() - 1);
    }
    
    // 用户 2 的 prompt（序列 1）
    std::vector<llama_token> prompt2 = common_tokenize(ctx, "Hi there", true);
    for (size_t i = 0; i < prompt2.size(); i++) {
        common_batch_add(batch, prompt2[i], i, {1}, i == prompt2.size() - 1);
    }
    
    // 批量处理两个用户的 prompt
    llama_decode(ctx, batch);
    
    // 分别采样
    llama_token token1 = common_sampler_sample(sampler1, ctx, prompt1.size() - 1);
    llama_token token2 = common_sampler_sample(sampler2, ctx, batch.n_tokens - 1);
    
    // 后续生成...
    
    // 清理
    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    
    return 0;
}
```

### 6.2 共享前缀优化

```cpp
// 场景：多个用户使用相同的系统 prompt

void process_shared_system_prompt(
    llama_context * ctx,
    llama_memory_t mem,
    const std::string & system_prompt,
    const std::vector<std::string> & user_prompts
) {
    llama_batch batch = llama_batch_init(4096, 0, 1);
    
    // 1. 处理系统 prompt 到序列 0
    auto sys_tokens = common_tokenize(ctx, system_prompt, true);
    for (size_t i = 0; i < sys_tokens.size(); i++) {
        common_batch_add(batch, sys_tokens[i], i, {0}, false);
    }
    llama_decode(ctx, batch);
    
    // 2. 复制系统 prompt 的 KV Cache 到所有用户序列
    for (size_t u = 0; u < user_prompts.size(); u++) {
        llama_memory_seq_cp(mem, 0, u + 1, -1, -1);
    }
    
    // 3. 处理各用户的输入
    common_batch_clear(batch);
    for (size_t u = 0; u < user_prompts.size(); u++) {
        auto user_tokens = common_tokenize(ctx, user_prompts[u], false);
        int base_pos = sys_tokens.size();
        for (size_t i = 0; i < user_tokens.size(); i++) {
            common_batch_add(batch, user_tokens[i], base_pos + i, 
                           {(int)(u + 1)}, i == user_tokens.size() - 1);
        }
    }
    
    llama_decode(ctx, batch);
    // ... 后续生成
}
```

## 七、注意事项

### 7.1 内存管理

- KV Cache 大小 = `n_ctx × n_layers × d_head × 2 (K+V) × sizeof(type)`
- 总内存需要考虑所有序列的 KV Cache 共享同一块内存区域
- 序列数量增加不会线性增加内存，但会增加碎片化风险

### 7.2 线程安全

- `llama_context` **不是线程安全的**
- 多线程访问需要外部同步
- Server 实现使用单线程事件循环 + 异步队列

### 7.3 序列 ID 限制

- 序列 ID 范围：`0` 到 `n_seq_max - 1`
- 超出限制的 `llama_decode` 会返回错误
- 需要合理规划序列 ID 的分配和回收

## 八、总结

llama.cpp 的并行请求处理机制基于以下核心概念：

| 概念 | 作用 |
|------|------|
| **Batch** | 将多个 token 打包批量处理 |
| **Sequence** | 隔离不同用户/请求的上下文 |
| **Memory/KV Cache** | 存储序列的注意力状态 |
| **Slot** | Server 层的请求管理单元 |
| **Task Queue** | 异步任务调度 |

通过这些机制的配合，llama.cpp 能够：
- 高效利用 GPU/CPU 并行计算能力
- 支持大量并发用户请求
- 实现低延迟、高吞吐的推理服务

## 参考资料

- [llama.cpp GitHub](https://github.com/ggml-org/llama.cpp)
- [llama.cpp Server PR #9283](https://github.com/ggml-org/llama.cpp/pull/9283) - 状态机设计
- [Continuous Batching 论文](https://arxiv.org/abs/2309.06180) - Orca: A Distributed Serving System for Transformer-Based Generative Models
