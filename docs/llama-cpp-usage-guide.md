# llama.cpp 调用指南与算子扩展

## 目录

1. [llama.cpp API 概述](#1-llamacpp-api-概述)
2. [核心调用流程](#2-核心调用流程)
3. [算法与 GGML 算子的映射关系](#3-算法与-ggml-算子的映射关系)
4. [扩展新算子](#4-扩展新算子)
5. [扩展新模型架构](#5-扩展新模型架构)
6. [实战示例](#6-实战示例)

---

## 1. llama.cpp API 概述

llama.cpp 是基于 GGML 构建的高效 LLM 推理框架，提供了 C API 接口供外部调用。

### 1.1 核心头文件

```c
#include "llama.h"       // 主 API 接口
#include "ggml.h"        // GGML 张量库
#include "ggml-backend.h" // 后端抽象层
#include "gguf.h"        // GGUF 模型格式
```

### 1.2 核心结构体

| 结构体 | 说明 |
|--------|------|
| `llama_model` | 模型对象，包含权重和配置 |
| `llama_context` | 推理上下文，包含 KV Cache 等 |
| `llama_vocab` | 词表对象 |
| `llama_batch` | 输入批次数据 |
| `llama_sampler` | 采样器 |

### 1.3 参数结构体

```c
// 模型加载参数
struct llama_model_params {
    ggml_backend_dev_t * devices;      // 使用的设备列表
    int32_t n_gpu_layers;              // GPU 层数
    enum llama_split_mode split_mode;  // 多 GPU 分割模式
    int32_t main_gpu;                  // 主 GPU
    const float * tensor_split;        // 张量分割比例
    llama_progress_callback progress_callback; // 进度回调
    void * progress_callback_user_data;
    const struct llama_model_kv_override * kv_overrides;
    bool vocab_only;      // 仅加载词表
    bool use_mmap;        // 使用内存映射
    bool use_mlock;       // 锁定内存
    bool check_tensors;   // 校验张量
};

// 上下文参数
struct llama_context_params {
    uint32_t n_ctx;             // 上下文长度
    uint32_t n_batch;           // 批次大小
    uint32_t n_ubatch;          // 微批次大小
    uint32_t n_seq_max;         // 最大序列数
    int32_t n_threads;          // 线程数
    int32_t n_threads_batch;    // 批处理线程数
    enum llama_rope_scaling_type rope_scaling_type;
    enum llama_pooling_type pooling_type;
    enum llama_attention_type attention_type;
    enum llama_flash_attn_type flash_attn_type;
    float rope_freq_base;
    float rope_freq_scale;
    enum ggml_type type_k;      // K cache 数据类型
    enum ggml_type type_v;      // V cache 数据类型
    bool embeddings;            // 是否输出 embedding
    bool offload_kqv;           // KQV 是否卸载到 GPU
    // ...
};
```

---

## 2. 核心调用流程

### 2.1 完整推理流程

```c
#include "llama.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char ** argv) {
    // 1. 初始化后端
    llama_backend_init();
    
    // 2. 加载模型
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 99;  // 全部卸载到 GPU
    
    llama_model * model = llama_model_load_from_file(
        "model.gguf", 
        model_params
    );
    if (!model) {
        fprintf(stderr, "Failed to load model\n");
        return 1;
    }
    
    // 3. 创建上下文
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 2048;
    ctx_params.n_batch = 512;
    ctx_params.n_threads = 4;
    
    llama_context * ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        llama_model_free(model);
        return 1;
    }
    
    // 4. 获取词表
    const llama_vocab * vocab = llama_model_get_vocab(model);
    
    // 5. tokenize
    const char * prompt = "Hello, world!";
    int n_tokens = -llama_vocab_tokenize(vocab, prompt, strlen(prompt), NULL, 0, false, true);
    llama_token * tokens = malloc(n_tokens * sizeof(llama_token));
    llama_vocab_tokenize(vocab, prompt, strlen(prompt), tokens, n_tokens, false, true);
    
    // 6. 创建 batch
    llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    
    // 7. 初始化采样器
    llama_sampler * sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    // 或者使用 temperature sampling
    // llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.8f));
    // llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));
    
    // 8. 预填充 (prefill)
    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "Failed to decode\n");
        return 1;
    }
    
    // 9. 生成循环
    int n_predict = 100;
    for (int i = 0; i < n_predict; i++) {
        // 获取 logits
        float * logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);
        
        // 采样下一个 token
        llama_token new_token = llama_sampler_sample(sampler, ctx, -1);
        
        // 检查是否结束
        if (llama_vocab_is_eog(vocab, new_token)) {
            break;
        }
        
        // 输出 token
        char buf[128];
        int n = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
        fwrite(buf, 1, n, stdout);
        fflush(stdout);
        
        // 准备下一个 batch
        batch = llama_batch_get_one(&new_token, 1);
        
        // 解码
        if (llama_decode(ctx, batch) != 0) {
            break;
        }
    }
    
    printf("\n");
    
    // 10. 清理资源
    llama_sampler_free(sampler);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    free(tokens);
    
    return 0;
}
```

### 2.2 使用 common 库简化调用

```cpp
#include "common.h"

int main(int argc, char ** argv) {
    // 解析命令行参数
    common_params params;
    params.model.path = "model.gguf";
    params.n_ctx = 2048;
    params.n_threads = 4;
    
    // 初始化
    common_init_result result = common_init_from_params(params);
    llama_model * model = result.model();
    llama_context * ctx = result.context();
    common_sampler * sampler = result.sampler(0);
    
    const llama_vocab * vocab = llama_model_get_vocab(model);
    
    // 生成文本
    std::string prompt = "Hello, world!";
    std::vector<llama_token> tokens;
    tokens.push_back(llama_vocab_bos(vocab));
    
    // tokenize
    // ...
    
    // 推理
    llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());
    llama_decode(ctx, batch);
    
    // 采样生成
    for (int i = 0; i < 100; i++) {
        llama_token new_token = common_sampler_sample(sampler, ctx, -1);
        if (llama_vocab_is_eog(vocab, new_token)) break;
        
        common_sampler_accept(sampler, new_token, true);
        
        char buf[128];
        llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
        printf("%s", buf);
        
        batch = llama_batch_get_one(&new_token, 1);
        llama_decode(ctx, batch);
    }
    
    return 0;
}
```

### 2.3 Embedding 提取

```c
// 启用 embedding 模式
ctx_params.embeddings = true;

// 创建上下文
llama_context * ctx = llama_init_from_model(model, ctx_params);

// 解码
llama_decode(ctx, batch);

// 获取 embeddings
float * embeddings = llama_get_embeddings(ctx);
// 或按序列获取
float * embd_seq = llama_get_embeddings_seq(ctx, seq_id);
```

---

## 3. 算法与 GGML 算子的映射关系

llama.cpp 的高层算法通过调用 GGML 底层算子实现。以下是主要映射关系：

### 3.1 Transformer 层算子映射

| 高层算法 | GGML 算子 | 说明 |
|----------|-----------|------|
| Token Embedding | `GGML_OP_CPY` | 将 token id 转换为向量 |
| Position Embedding (RoPE) | `GGML_OP_ROPE` | 旋转位置编码 |
| RMS Norm | `GGML_OP_RMS_NORM` | RMS 归一化 |
| QKV 投影 | `GGML_OP_MUL_MAT` | 矩阵乘法 |
| 注意力分数 | `GGML_OP_MUL_MAT` + `GGML_OP_SOFT_MAX` | Q@K^T * scale + softmax |
| Flash Attention | `GGML_OP_FLASH_ATTN_EXT` | 高效注意力实现 |
| FFN (SwiGLU) | `GGML_OP_MUL_MAT` + `GGML_OP_SILU` + `GGML_OP_MUL` | 门控前馈网络 |
| 残差连接 | `GGML_OP_ADD` | 加法残差 |

### 3.2 详细调用链

以 LLaMA 的一个 Transformer 层为例：

```
┌─────────────────────────────────────────────────────────────────┐
│                    LLaMA Transformer Layer                       │
├─────────────────────────────────────────────────────────────────┤
│  Input Tensor: inpL [n_embd, n_tokens]                          │
│                                                                   │
│  1. RMS Norm                                                      │
│     └─> ggml_rms_norm(ctx, inpL, eps)                            │
│         └─> GGML_OP_RMS_NORM                                      │
│                                                                   │
│  2. QKV 投影                                                      │
│     └─> ggml_mul_mat(ctx, wq, normed)  → Qcur                    │
│     └─> ggml_mul_mat(ctx, wk, normed)  → Kcur                    │
│     └─> ggml_mul_mat(ctx, wv, normed)  → Vcur                    │
│         └─> GGML_OP_MUL_MAT                                      │
│                                                                   │
│  3. RoPE 位置编码                                                 │
│     └─> ggml_rope_ext(ctx, Qcur, pos, ...)                       │
│     └─> ggml_rope_ext(ctx, Kcur, pos, ...)                       │
│         └─> GGML_OP_ROPE                                         │
│                                                                   │
│  4. Flash Attention                                               │
│     └─> ggml_flash_attn_ext(ctx, Q, K, V, mask, scale, ...)      │
│         └─> GGML_OP_FLASH_ATTN_EXT                               │
│                                                                   │
│  5. 输出投影                                                      │
│     └─> ggml_mul_mat(ctx, wo, attn_out)                          │
│         └─> GGML_OP_MUL_MAT                                      │
│                                                                   │
│  6. 残差连接                                                      │
│     └─> ggml_add(ctx, attn_out, inpL) → ffn_inp                  │
│         └─> GGML_OP_ADD                                          │
│                                                                   │
│  7. FFN (SwiGLU)                                                  │
│     └─> ggml_rms_norm(ctx, ffn_inp, eps)                         │
│     └─> ggml_mul_mat(ctx, ffn_gate, normed) → gate               │
│     └─> ggml_mul_mat(ctx, ffn_up, normed)   → up                 │
│     └─> ggml_silu(ctx, gate)                                     │
│         └─> GGML_OP_UNARY (SILU)                                 │
│     └─> ggml_mul(ctx, gate, up) → hidden                         │
│         └─> GGML_OP_MUL                                          │
│     └─> ggml_mul_mat(ctx, ffn_down, hidden) → ffn_out            │
│         └─> GGML_OP_MUL_MAT                                      │
│                                                                   │
│  8. 最终残差                                                      │
│     └─> ggml_add(ctx, ffn_out, ffn_inp) → cur                    │
│         └─> GGML_OP_ADD                                          │
│                                                                   │
│  Output Tensor: cur [n_embd, n_tokens]                           │
└─────────────────────────────────────────────────────────────────┘
```

### 3.3 后端算子实现映射

每个 GGML 算子在不同后端有不同实现：

```
GGML_OP_MUL_MAT
├── CPU:  ggml-cpu/ops.cpp       → ggml_compute_forward_mul_mat()
├── CUDA: ggml-cuda/mmq.cu       → ggml_cuda_op_mul_mat()
├── Metal: ggml-metal.metal      → kernel_mul_mm()
├── Vulkan: ggml-vulkan/...      → shader 实现
└── SYCL: ggml-sycl/...          → SYCL kernel

GGML_OP_FLASH_ATTN_EXT
├── CPU:  ggml-cpu/ops.cpp       → 软件实现
├── CUDA: ggml-cuda/fattn/...    → Flash Attention CUDA kernel
├── Metal: ggml-metal.metal      → Metal Flash Attention
└── 其他后端: fallback 或不支持
```

---

## 4. 扩展新算子

扩展新算子有两种主要方式：使用自定义算子 API 或添加新的原生算子。

### 4.1 方式一：使用自定义算子 API

GGML 提供了 `ggml_map_custom*` 系列函数用于注册自定义算子。

#### 自定义算子函数签名

```c
// 单输入自定义算子
typedef void (*ggml_custom1_op_t)(
    struct ggml_tensor * dst, 
    const struct ggml_tensor * a, 
    int ith,    // 线程索引
    int nth,    // 总线程数
    void * userdata
);

// 双输入自定义算子
typedef void (*ggml_custom2_op_t)(
    struct ggml_tensor * dst, 
    const struct ggml_tensor * a, 
    const struct ggml_tensor * b, 
    int ith, int nth, 
    void * userdata
);

// 三输入自定义算子
typedef void (*ggml_custom3_op_t)(
    struct ggml_tensor * dst, 
    const struct ggml_tensor * a, 
    const struct ggml_tensor * b, 
    const struct ggml_tensor * c, 
    int ith, int nth, 
    void * userdata
);
```

#### 示例：实现自定义激活函数

```c
#include "ggml.h"
#include <math.h>

// 自定义激活函数: Mish(x) = x * tanh(softplus(x))
// softplus(x) = ln(1 + exp(x))
void my_mish_op(
    struct ggml_tensor * dst, 
    const struct ggml_tensor * a, 
    int ith, int nth, 
    void * userdata
) {
    GGML_UNUSED(userdata);
    
    const int64_t ne = ggml_nelements(a);
    const int64_t dr = (ne + nth - 1) / nth;
    const int64_t ie0 = dr * ith;
    const int64_t ie1 = MIN(ie0 + dr, ne);
    
    const float * src = (const float *) a->data;
    float * dst_data = (float *) dst->data;
    
    for (int64_t i = ie0; i < ie1; i++) {
        float x = src[i];
        float sp = logf(1.0f + expf(x));  // softplus
        dst_data[i] = x * tanhf(sp);       // mish
    }
}

// 使用自定义算子
struct ggml_tensor * my_mish(
    struct ggml_context * ctx, 
    struct ggml_tensor * x
) {
    // 创建输出张量（与输入相同形状）
    struct ggml_tensor * result = ggml_map_custom1(
        ctx,
        x,
        my_mish_op,
        GGML_N_TASKS_MAX,  // 使用最大线程数
        NULL                // 无用户数据
    );
    return result;
}
```

#### 示例：带参数的自定义算子

```c
// 用户数据结构
typedef struct {
    float alpha;
    float beta;
} my_leaky_relu_params;

// Leaky ReLU: max(alpha * x, beta * x)
void my_leaky_relu_op(
    struct ggml_tensor * dst, 
    const struct ggml_tensor * a, 
    int ith, int nth, 
    void * userdata
) {
    my_leaky_relu_params * params = (my_leaky_relu_params *) userdata;
    
    const int64_t ne = ggml_nelements(a);
    const int64_t dr = (ne + nth - 1) / nth;
    const int64_t ie0 = dr * ith;
    const int64_t ie1 = MIN(ie0 + dr, ne);
    
    const float * src = (const float *) a->data;
    float * dst_data = (float *) dst->data;
    
    const float alpha = params->alpha;
    const float beta = params->beta;
    
    for (int64_t i = ie0; i < ie1; i++) {
        float x = src[i];
        dst_data[i] = x > 0 ? beta * x : alpha * x;
    }
}

// 封装函数
struct ggml_tensor * my_leaky_relu(
    struct ggml_context * ctx,
    struct ggml_tensor * x,
    float alpha,
    float beta
) {
    // 注意：params 必须在计算图执行期间保持有效
    static my_leaky_relu_params params;
    params.alpha = alpha;
    params.beta = beta;
    
    return ggml_map_custom1(ctx, x, my_leaky_relu_op, GGML_N_TASKS_MAX, &params);
}
```

#### 示例：双输入自定义算子（逐元素运算）

```c
// 自定义加权融合: alpha * a + beta * b + gamma * a * b
typedef struct {
    float alpha;
    float beta;
    float gamma;
} my_fusion_params;

void my_fusion_op(
    struct ggml_tensor * dst,
    const struct ggml_tensor * a,
    const struct ggml_tensor * b,
    int ith, int nth,
    void * userdata
) {
    my_fusion_params * p = (my_fusion_params *) userdata;
    
    const int64_t ne = ggml_nelements(a);
    const int64_t dr = (ne + nth - 1) / nth;
    const int64_t ie0 = dr * ith;
    const int64_t ie1 = MIN(ie0 + dr, ne);
    
    const float * src_a = (const float *) a->data;
    const float * src_b = (const float *) b->data;
    float * dst_data = (float *) dst->data;
    
    for (int64_t i = ie0; i < ie1; i++) {
        dst_data[i] = p->alpha * src_a[i] + p->beta * src_b[i] + p->gamma * src_a[i] * src_b[i];
    }
}

struct ggml_tensor * my_fusion(
    struct ggml_context * ctx,
    struct ggml_tensor * a,
    struct ggml_tensor * b,
    float alpha, float beta, float gamma
) {
    static my_fusion_params params = {0};
    params.alpha = alpha;
    params.beta = beta;
    params.gamma = gamma;
    
    return ggml_map_custom2(ctx, a, b, my_fusion_op, GGML_N_TASKS_MAX, &params);
}
```

### 4.2 方式二：添加原生 GGML 算子

如果需要更高性能或更多后端支持，可以添加原生算子。

#### 步骤 1：在 ggml.h 中定义算子枚举

```c
// 在 enum ggml_op 中添加
enum ggml_op {
    // ... 现有算子
    
    GGML_OP_MY_CUSTOM,  // 新算子
    
    GGML_OP_COUNT,
};
```

#### 步骤 2：在 ggml.c 中实现算子

```c
// 前向计算实现
static void ggml_compute_forward_my_custom(
    struct ggml_compute_params * params,
    struct ggml_tensor * dst
) {
    const struct ggml_tensor * src0 = dst->src[0];
    
    const int64_t ne = ggml_nelements(src0);
    const int nth = params->nth;
    const int ith = params->ith;
    const int64_t dr = (ne + nth - 1) / nth;
    const int64_t ie0 = dr * ith;
    const int64_t ie1 = MIN(ie0 + dr, ne);
    
    const float * src = (const float *) src0->data;
    float * dst_data = (float *) dst->data;
    
    // 实现具体计算逻辑
    for (int64_t i = ie0; i < ie1; i++) {
        dst_data[i] = src[i] * 2.0f;  // 示例：乘以 2
    }
}

// 在 ggml_compute_forward 中添加调用
static void ggml_compute_forward(struct ggml_compute_params * params, struct ggml_tensor * tensor) {
    switch (tensor->op) {
        // ... 其他 case
        case GGML_OP_MY_CUSTOM:
            ggml_compute_forward_my_custom(params, tensor);
            break;
        default:
            GGML_ABORT("fatal error");
    }
}
```

#### 步骤 3：添加创建函数

```c
// 在 ggml.h 声明
GGML_API struct ggml_tensor * ggml_my_custom(
    struct ggml_context * ctx,
    struct ggml_tensor * a
);

// 在 ggml.c 实现
struct ggml_tensor * ggml_my_custom(
    struct ggml_context * ctx,
    struct ggml_tensor * a
) {
    struct ggml_tensor * result = ggml_new_tensor(ctx, a->type, GGML_MAX_DIMS, a->ne);
    result->op = GGML_OP_MY_CUSTOM;
    result->src[0] = a;
    return result;
}
```

#### 步骤 4：为 GPU 后端添加实现（可选）

在 `ggml-cuda/` 目录下添加 CUDA 实现：

```cpp
// ggml-cuda/my_custom.cuh
#pragma once
#include "common.cuh"

void ggml_cuda_op_my_custom(ggml_backend_cuda_context & ctx, ggml_tensor * dst);

// ggml-cuda/my_custom.cu
#include "my_custom.cuh"

static __global__ void my_custom_kernel(const float * x, float * dst, const int64_t n) {
    int64_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        dst[i] = x[i] * 2.0f;
    }
}

void ggml_cuda_op_my_custom(ggml_backend_cuda_context & ctx, ggml_tensor * dst) {
    const ggml_tensor * src0 = dst->src[0];
    const int64_t ne = ggml_nelements(src0);
    
    const float * src_d = (const float *) src0->data;
    float * dst_d = (float *) dst->data;
    
    const int n_blocks = (ne + CUDA_MY_CUSTOM_BLOCK_SIZE - 1) / CUDA_MY_CUSTOM_BLOCK_SIZE;
    
    my_custom_kernel<<<n_blocks, CUDA_MY_CUSTOM_BLOCK_SIZE, 0, ctx.stream()>>>(
        src_d, dst_d, ne
    );
}
```

---

## 5. 扩展新模型架构

### 5.1 整体流程

1. **转换模型为 GGUF 格式**（Python）
2. **定义模型架构枚举**（llama-arch.h）
3. **注册张量名称**（llama-arch.cpp）
4. **实现计算图构建**（llama-model.cpp）

### 5.2 步骤详解

#### 步骤 1：定义架构枚举

在 `src/llama-arch.h` 中添加：

```cpp
enum llm_arch {
    LLM_ARCH_UNKNOWN = -1,
    LLM_ARCH_LLAMA,
    // ... 现有架构
    LLM_ARCH_MY_MODEL,  // 新架构
};
```

#### 步骤 2：定义张量名称

在 `src/llama-arch.cpp` 中添加：

```cpp
// 架构名称映射
static const std::map<llm_arch, std::string> LLM_ARCH_NAMES = {
    // ... 现有映射
    { LLM_ARCH_MY_MODEL,      "my_model"      },
};

// 张量名称列表
static const std::map<llm_arch, std::map<llm_tensor, std::string>> LLM_TENSOR_NAMES = {
    // ... 现有映射
    {
        LLM_ARCH_MY_MODEL, {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_out" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
        }
    },
};
```

#### 步骤 3：实现计算图构建

在 `src/llama-model.cpp` 中添加：

```cpp
// 定义图构建类
struct llm_build_my_model : public llm_graph_context {
    llm_build_my_model(const llama_model & model, const llm_graph_params & params) 
        : llm_graph_context(params) 
    {
        // 获取超参数
        const int64_t n_embd_head = hparams.n_embd_head_v();
        
        // 张量名称辅助
        const LLM_TN tn(LLM_ARCH_MY_MODEL);
        
        ggml_tensor * cur;
        ggml_tensor * inpL;
        
        // 1. Token Embedding
        inpL = build_inp_embd(model.tok_embd);
        
        // 2. 位置输入
        ggml_tensor * inp_pos = build_inp_pos();
        
        // 3. 注意力输入
        auto * inp_attn = build_attn_inp_kv();
        
        // 4. 遍历层
        for (int il = 0; il < n_layer; ++il) {
            ggml_tensor * inpSA = inpL;
            
            // 4.1 注意力归一化
            cur = build_norm(inpL, model.layers[il].attn_norm, NULL, LLM_NORM_RMS, il);
            cb(cur, "attn_norm", il);
            
            // 4.2 计算 Q, K, V
            ggml_tensor * Qcur = ggml_mul_mat(ctx0, model.layers[il].wq, cur);
            ggml_tensor * Kcur = ggml_mul_mat(ctx0, model.layers[il].wk, cur);
            ggml_tensor * Vcur = ggml_mul_mat(ctx0, model.layers[il].wv, cur);
            
            cb(Qcur, "Qcur", il);
            cb(Kcur, "Kcur", il);
            cb(Vcur, "Vcur", il);
            
            // 4.3 应用 RoPE
            Qcur = ggml_rope_ext(ctx0, Qcur, inp_pos, nullptr, n_rot, rope_type, 
                                 n_ctx_orig, freq_base, freq_scale, 
                                 ext_factor, attn_factor, beta_fast, beta_slow);
            Kcur = ggml_rope_ext(ctx0, Kcur, inp_pos, nullptr, n_rot, rope_type, 
                                 n_ctx_orig, freq_base, freq_scale, 
                                 ext_factor, attn_factor, beta_fast, beta_slow);
            
            // 4.4 注意力计算
            cur = build_attn(inp_attn,
                    model.layers[il].wo, NULL,
                    Qcur, Kcur, Vcur, nullptr, nullptr, nullptr, 
                    1.0f / sqrtf(float(n_embd_head)), il);
            
            // 4.5 残差连接
            ggml_tensor * ffn_inp = ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);
            
            // 4.6 FFN 归一化
            cur = build_norm(ffn_inp, model.layers[il].ffn_norm, NULL, LLM_NORM_RMS, il);
            cb(cur, "ffn_norm", il);
            
            // 4.7 FFN (SwiGLU)
            cur = build_ffn(cur,
                    model.layers[il].ffn_up, NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL, LLM_FFN_SWIGLU, LLM_FFN_PAR, il);
            cb(cur, "ffn_out", il);
            
            // 4.8 残差连接
            cur = ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "l_out", il);
            
            // 输入到下一层
            inpL = cur;
        }
        
        // 5. 最终归一化
        cur = build_norm(inpL, model.output_norm, NULL, LLM_NORM_RMS, -1);
        cb(cur, "result_norm", -1);
        res->t_embd = cur;
        
        // 6. 输出投影
        cur = build_lora_mm(model.output, cur);
        cb(cur, "result_output", -1);
        res->t_logits = cur;
        
        // 构建计算图
        ggml_build_forward_expand(gf, cur);
    }
};

// 在 llama_model::build_graph 中注册
ggml_cgraph * llama_model::build_graph(...) {
    // ...
    switch (arch) {
        // ... 其他 case
        case LLM_ARCH_MY_MODEL:
            llm = std::make_unique<llm_build_my_model>(*this, params);
            break;
    }
    // ...
}
```

### 5.3 Python 转换脚本

在 `convert_hf_to_gguf.py` 中添加：

```python
@ModelBase.register("MyModelForCausalLM")
class MyModel(TextModel):
    model_arch = gguf.MODEL_ARCH_MY_MODEL

    def set_gguf_parameters(self):
        super().set_gguf_parameters()
        # 添加自定义参数
        self.gguf.add_bool("my_custom_flag", True)
    
    def set_vocab(self):
        # 设置词表
        pass
    
    def write_tensors(self):
        # 写入张量
        for name, data in self.get_tensors():
            # 转换张量名称
            new_name = self.map_tensor_name(name)
            self.write_tensor(new_name, data)
```

---

## 6. 实战示例

### 6.1 完整的自定义算子示例

实现一个带缩放的 Softplus 激活函数：`softplus(x) * scale`

```cpp
// my_custom_ops.h
#pragma once
#include "ggml.h"

#ifdef __cplusplus
extern "C" {
#endif

// 自定义激活函数参数
struct my_softplus_params {
    float scale;
    float threshold;  // 数值稳定性阈值
};

// 创建 Softplus 张量
GGML_API struct ggml_tensor * ggml_my_softplus(
    struct ggml_context * ctx,
    struct ggml_tensor * a,
    float scale,
    float threshold
);

#ifdef __cplusplus
}
#endif
```

```cpp
// my_custom_ops.cpp
#include "my_custom_ops.h"
#include <math.h>
#include <float.h>

// 内部实现函数
static void ggml_compute_forward_my_softplus(
    struct ggml_compute_params * params,
    struct ggml_tensor * dst
) {
    const struct ggml_tensor * src0 = dst->src[0];
    
    // 获取参数
    float scale = ggml_get_op_params_f32(dst, 0);
    float threshold = ggml_get_op_params_f32(dst, 1);
    
    const int64_t ne = ggml_nelements(src0);
    const int nth = params->nth;
    const int ith = params->ith;
    
    const int64_t dr = (ne + nth - 1) / nth;
    const int64_t ie0 = dr * ith;
    const int64_t ie1 = MIN(ie0 + dr, ne);
    
    const float * src = (const float *) src0->data;
    float * dst_data = (float *) dst->data;
    
    // 使用数值稳定的 softplus 实现
    // softplus(x) = log(1 + exp(x))
    // 当 x 很大时: softplus(x) ≈ x
    // 当 x 很小时: softplus(x) ≈ exp(x)
    for (int64_t i = ie0; i < ie1; i++) {
        float x = src[i];
        float result;
        
        if (x > threshold) {
            result = x;  // 避免数值溢出
        } else if (x < -threshold) {
            result = expf(x);
        } else {
            result = logf(1.0f + expf(x));
        }
        
        dst_data[i] = result * scale;
    }
}

// API 实现
struct ggml_tensor * ggml_my_softplus(
    struct ggml_context * ctx,
    struct ggml_tensor * a,
    float scale,
    float threshold
) {
    struct ggml_tensor * result = ggml_new_tensor(ctx, a->type, GGML_MAX_DIMS, a->ne);
    
    result->op = GGML_OP_MY_SOFTPLUS;
    result->src[0] = a;
    
    // 存储参数
    ggml_set_op_params_f32(result, 0, scale);
    ggml_set_op_params_f32(result, 1, threshold);
    
    return result;
}
```

### 6.2 在模型中使用自定义算子

```cpp
// 在自定义模型中使用
struct llm_build_my_custom_model : public llm_graph_context {
    llm_build_my_custom_model(const llama_model & model, const llm_graph_params & params)
        : llm_graph_context(params)
    {
        // ... 其他层
        
        // 使用自定义激活函数
        ggml_tensor * activated = ggml_my_softplus(
            ctx0, 
            hidden,
            1.5f,    // scale
            20.0f    // threshold
        );
        
        // ... 继续构建
    }
};
```

### 6.3 编译和链接

```cmake
# CMakeLists.txt
add_library(my_custom_ops STATIC my_custom_ops.cpp)

# 如果添加到 llama.cpp 构建系统
target_link_libraries(llama PRIVATE my_custom_ops)
```

---

## 附录：API 速查表

### 核心 API

| 函数 | 说明 |
|------|------|
| `llama_backend_init()` | 初始化后端 |
| `llama_backend_free()` | 释放后端 |
| `llama_model_load_from_file()` | 从文件加载模型 |
| `llama_model_free()` | 释放模型 |
| `llama_init_from_model()` | 从模型创建上下文 |
| `llama_free()` | 释放上下文 |
| `llama_decode()` | 执行推理 |
| `llama_get_logits()` | 获取 logits |
| `llama_get_embeddings()` | 获取 embeddings |

### 采样 API

| 函数 | 说明 |
|------|------|
| `llama_sampler_chain_init()` | 初始化采样器链 |
| `llama_sampler_init_greedy()` | 贪婪采样 |
| `llama_sampler_init_temp()` | 温度采样 |
| `llama_sampler_init_top_k()` | Top-K 采样 |
| `llama_sampler_init_top_p()` | Top-P (Nucleus) 采样 |
| `llama_sampler_init_min_p()` | Min-P 采样 |
| `llama_sampler_init_mirostat()` | Mirostat 采样 |
| `llama_sampler_sample()` | 执行采样 |

### Batch API

| 函数 | 说明 |
|------|------|
| `llama_batch_init()` | 初始化 batch |
| `llama_batch_free()` | 释放 batch |
| `llama_batch_get_one()` | 创建单 token batch |

### GGML 自定义算子 API

| 函数 | 说明 |
|------|------|
| `ggml_map_custom1()` | 单输入自定义算子 |
| `ggml_map_custom2()` | 双输入自定义算子 |
| `ggml_map_custom3()` | 三输入自定义算子 |
| `ggml_custom_4d()` | 通用自定义算子 |
