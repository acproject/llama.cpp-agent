# GGML 算子分析与使用指南

## 目录

1. [GGML 概述](#1-ggml-概述)
2. [核心数据结构](#2-核心数据结构)
3. [算子分类与详解](#3-算子分类与详解)
4. [llama.cpp 中的算子使用](#4-llamacpp-中的算子使用)
5. [后端架构](#5-后端架构)
6. [量化类型](#6-量化类型)

---

## 1. GGML 概述

GGML (Georgi Gerganov Machine Learning) 是一个用于机器学习的张量运算库，具有以下核心特性：

- **张量运算**：支持多达 4 维的张量操作
- **自动微分**：支持自动计算梯度
- **优化算法**：内置 AdamW、SGD 等优化器
- **量化支持**：支持多种量化格式（Q4_0、Q8_0、K-quants 等）
- **多后端**：支持 CPU、CUDA、Metal、Vulkan、SYCL 等多种后端
- **计算图**：通过计算图进行惰性求值

### 基本使用流程

```c
// 1. 初始化上下文
struct ggml_init_params params = {
    .mem_size   = 16*1024*1024,
    .mem_buffer = NULL,
};
struct ggml_context * ctx = ggml_init(params);

// 2. 创建张量
struct ggml_tensor * x = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 1);
ggml_set_param(ctx, x);  // 标记为可训练参数

// 3. 构建计算图
struct ggml_tensor * a  = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 1);
struct ggml_tensor * b  = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 1);
struct ggml_tensor * x2 = ggml_mul(ctx, x, x);
struct ggml_tensor * f  = ggml_add(ctx, ggml_mul(ctx, a, x2), b);

// 4. 创建计算图并执行
struct ggml_cgraph * gf = ggml_new_graph(ctx);
ggml_build_forward_expand(gf, f);
ggml_graph_compute_with_ctx(ctx, &gf, n_threads);
```

---

## 2. 核心数据结构

### 2.1 ggml_tensor

```c
struct ggml_tensor {
    enum ggml_type type;              // 数据类型（F32, F16, Q4_0 等）
    struct ggml_backend_buffer * buffer; // 后端缓冲区
    
    int64_t ne[GGML_MAX_DIMS];        // 各维度元素数量 [ne0, ne1, ne2, ne3]
    size_t  nb[GGML_MAX_DIMS];        // 各维度字节步长
    
    enum ggml_op op;                  // 操作类型
    int32_t op_params[GGML_MAX_OP_PARAMS / sizeof(int32_t)]; // 操作参数
    int32_t flags;                    // 张量标志
    
    struct ggml_tensor * src[GGML_MAX_SRC]; // 源张量数组
    
    struct ggml_tensor * view_src;    // 视图源张量
    size_t view_offs;                 // 视图偏移
    
    void * data;                      // 数据指针
    char name[GGML_MAX_NAME];         // 张量名称
    void * extra;                     // 额外数据（如 CUDA 使用）
};
```

### 2.2 张量标志

```c
enum ggml_tensor_flag {
    GGML_TENSOR_FLAG_INPUT   =  1,  // 输入张量
    GGML_TENSOR_FLAG_OUTPUT  =  2,  // 输出张量
    GGML_TENSOR_FLAG_PARAM   =  4,  // 可训练参数
    GGML_TENSOR_FLAG_LOSS    =  8,  // 损失函数
    GGML_TENSOR_FLAG_COMPUTE = 16,  // 需要计算
};
```

---

## 3. 算子分类与详解

GGML 提供了超过 100 种算子，按功能可分为以下几类：

### 3.1 基础数学运算

| 算子 | 函数签名 | 说明 |
|------|----------|------|
| `GGML_OP_ADD` | `ggml_add(ctx, a, b)` | 逐元素加法 |
| `GGML_OP_SUB` | `ggml_sub(ctx, a, b)` | 逐元素减法 |
| `GGML_OP_MUL` | `ggml_mul(ctx, a, b)` | 逐元素乘法 |
| `GGML_OP_DIV` | `ggml_div(ctx, a, b)` | 逐元素除法 |
| `GGML_OP_SQR` | `ggml_sqr(ctx, a)` | 平方 |
| `GGML_OP_SQRT` | `ggml_sqrt(ctx, a)` | 平方根 |
| `GGML_OP_LOG` | `ggml_log(ctx, a)` | 自然对数 |
| `GGML_OP_SIN` | `ggml_sin(ctx, a)` | 正弦函数 |
| `GGML_OP_COS` | `ggml_cos(ctx, a)` | 余弦函数 |

**使用示例：**

```c
// 逐元素加法：c = a + b
struct ggml_tensor * c = ggml_add(ctx, a, b);

// 原地操作版本（返回视图）
struct ggml_tensor * c = ggml_add_inplace(ctx, a, b);

// 带类型转换的加法
struct ggml_tensor * c = ggml_add_cast(ctx, a, b, GGML_TYPE_F16);
```

### 3.2 激活函数（一元运算）

```c
enum ggml_unary_op {
    GGML_UNARY_OP_ABS,        // 绝对值
    GGML_UNARY_OP_SGN,        // 符号函数
    GGML_UNARY_OP_NEG,        // 取负
    GGML_UNARY_OP_TANH,       // 双曲正切
    GGML_UNARY_OP_ELU,        // ELU 激活
    GGML_UNARY_OP_RELU,       // ReLU 激活
    GGML_UNARY_OP_SIGMOID,    // Sigmoid 激活
    GGML_UNARY_OP_GELU,       // GELU 激活
    GGML_UNARY_OP_GELU_QUICK, // 快速 GELU 近似
    GGML_UNARY_OP_SILU,       // SiLU/Swish 激活
    GGML_UNARY_OP_EXP,        // 指数函数
    GGML_UNARY_OP_FLOOR,      // 向下取整
    GGML_UNARY_OP_CEIL,       // 向上取整
    // ...
};
```

**使用示例：**

```c
// ReLU 激活
struct ggml_tensor * y = ggml_relu(ctx, x);
struct ggml_tensor * y = ggml_relu_inplace(ctx, x);

// GELU 激活（用于 Transformer）
struct ggml_tensor * y = ggml_gelu(ctx, x);

// SiLU 激活（用于 LLaMA 的 FFN）
struct ggml_tensor * y = ggml_silu(ctx, x);
```

### 3.3 归一化运算

| 算子 | 函数 | 说明 |
|------|------|------|
| `GGML_OP_NORM` | `ggml_norm(ctx, a, eps)` | 层归一化 |
| `GGML_OP_RMS_NORM` | `ggml_rms_norm(ctx, a, eps)` | RMS 归一化 |
| `GGML_OP_GROUP_NORM` | `ggml_group_norm(ctx, a, n_groups, eps)` | 组归一化 |
| `GGML_OP_L2_NORM` | `ggml_l2_norm(ctx, a, eps)` | L2 归一化 |

**RMS Norm 示例（LLaMA 使用）：**

```c
// RMS Norm: y = x * rsqrt(mean(x^2) + eps)
struct ggml_tensor * normalized = ggml_rms_norm(ctx, x, 1e-5f);

// 应用缩放权重
struct ggml_tensor * y = ggml_mul(ctx, normalized, weight);
```

### 3.4 矩阵运算

| 算子 | 函数 | 说明 |
|------|------|------|
| `GGML_OP_MUL_MAT` | `ggml_mul_mat(ctx, a, b)` | 矩阵乘法（核心算子） |
| `GGML_OP_MUL_MAT_ID` | `ggml_mul_mat_id(ctx, as, b, ids)` | 间接矩阵乘法（MoE） |
| `GGML_OP_OUT_PROD` | `ggml_out_prod(ctx, a, b)` | 外积 |
| `GGML_OP_TRANSPOSE` | `ggml_transpose(ctx, a)` | 转置 |

**矩阵乘法详解：**

```c
// 矩阵乘法: result = A @ B^T
// A: [k, n] - 左矩阵
// B: [k, m] - 右矩阵（内部转置）
// result: [n, m]
struct ggml_tensor * result = ggml_mul_mat(ctx, A, B);

// 设置高精度计算
ggml_mul_mat_set_prec(result, GGML_PREC_F32);
```

**矩阵乘法在 Transformer 中的应用：**

```c
// Q = X @ Wq^T
struct ggml_tensor * Q = ggml_mul_mat(ctx, wq, input);

// K = X @ Wk^T
struct ggml_tensor * K = ggml_mul_mat(ctx, wk, input);

// V = X @ Wv^T
struct ggml_tensor * V = ggml_mul_mat(ctx, wv, input);
```

### 3.5 注意力机制算子

#### 3.5.1 Softmax

```c
// 基础 softmax
struct ggml_tensor * probs = ggml_soft_max(ctx, logits);

// 带缩放和掩码的 softmax（用于注意力）
struct ggml_tensor * attn_weights = ggml_soft_max_ext(
    ctx,
    logits,      // 输入
    mask,        // 注意力掩码（可选）
    scale,       // 缩放因子（通常 1/sqrt(d_k)）
    max_bias     // ALiBi 偏置（0.0 表示不使用）
);
```

#### 3.5.2 RoPE（旋转位置编码）

```c
// 基础 RoPE
struct ggml_tensor * q_rope = ggml_rope(
    ctx,
    q,            // 输入张量
    positions,    // 位置向量
    n_rot,        // 旋转维度数
    mode          // RoPE 类型
);

// 扩展 RoPE（支持更多参数）
struct ggml_tensor * q_rope = ggml_rope_ext(
    ctx,
    q,
    positions,
    freq_factors, // 频率因子（可选）
    n_rot,
    mode,
    n_ctx_orig,   // 原始上下文长度
    freq_base,    // 频率基数
    freq_scale,   // 频率缩放
    ext_factor,   // 扩展因子
    attn_factor,  // 注意力因子
    beta_fast,
    beta_slow
);

// RoPE 类型定义
#define GGML_ROPE_TYPE_NORMAL 0   // 标准 RoPE
#define GGML_ROPE_TYPE_NEOX   2   // GPT-NeoX 风格
#define GGML_ROPE_TYPE_MROPE  8   // 多模态 RoPE
#define GGML_ROPE_TYPE_VISION 24  // 视觉 RoPE
```

#### 3.5.3 Flash Attention

```c
// Flash Attention（高性能注意力实现）
// q: [n_embd_h, n_head, n_batch, ne3]
// k: [n_embd_v, n_kv, n_head_kv, ne3]
// v: [n_embd_v, n_kv, n_head_kv, ne3]
// mask: [n_kv, n_batch, ne32, ne33]
// result: [n_embd_v, n_head, n_batch, ne3]
struct ggml_tensor * attn_out = ggml_flash_attn_ext(
    ctx,
    q, k, v, mask,
    scale,        // 缩放因子
    max_bias,     // ALiBi 偏置
    logit_softcap // logit 软上限
);

// 设置精度
ggml_flash_attn_ext_set_prec(attn_out, GGML_PREC_F32);
```

#### 3.5.4 注意力掩码

```c
// 因果掩码（将对角线上方设为 -INF）
struct ggml_tensor * masked = ggml_diag_mask_inf(ctx, attn, n_past);

// 零掩码（将对角线上方设为 0）
struct ggml_tensor * masked = ggml_diag_mask_zero(ctx, attn, n_past);
```

### 3.6 形状操作

| 算子 | 函数 | 说明 |
|------|------|------|
| `GGML_OP_RESHAPE` | `ggml_reshape_*d(ctx, a, ...)` | 改变形状 |
| `GGML_OP_VIEW` | `ggml_view_*d(ctx, a, ...)` | 创建视图 |
| `GGML_OP_PERMUTE` | `ggml_permute(ctx, a, axis0, axis1, axis2, axis3)` | 维度重排 |
| `GGML_OP_TRANSPOSE` | `ggml_transpose(ctx, a)` | 转置 |
| `GGML_OP_CONT` | `ggml_cont(ctx, a)` | 使内存连续 |
| `GGML_OP_CPY` | `ggml_cpy(ctx, a, b)` | 复制数据 |

**使用示例：**

```c
// 重塑为 3D 张量
struct ggml_tensor * reshaped = ggml_reshape_3d(ctx, x, dim0, dim1, dim2);

// 视图操作（不复制数据）
struct ggml_tensor * view = ggml_view_2d(ctx, x, ne0, ne1, nb1, offset);

// 维度重排（用于注意力头分离）
// 将 [n_embd, n_tokens] 变为 [n_embd_head, n_head, n_tokens]
struct ggml_tensor * permuted = ggml_permute(ctx, q, 0, 2, 1, 3);
```

### 3.7 索引与选择

```c
// 按行索引获取
// a: [n_embd, n_rows, ...]
// indices: [n_selected, ...] (I32 类型)
// result: [n_embd, n_selected, ...]
struct ggml_tensor * selected = ggml_get_rows(ctx, a, indices);

// 按行设置
struct ggml_tensor * updated = ggml_set_rows(ctx, dst, src, indices);
```

### 3.8 卷积运算

```c
// 1D 卷积
struct ggml_tensor * conv1d = ggml_conv_1d(ctx, kernel, data, stride, padding, dilation);

// 2D 卷积
struct ggml_tensor * conv2d = ggml_conv_2d(
    ctx, kernel, data,
    stride0, stride1,
    padding0, padding1,
    dilation0, dilation1
);

// 深度可分离卷积
struct ggml_tensor * dw_conv = ggml_conv_2d_dw(ctx, kernel, data, s0, s1, p0, p1, d0, d1);

// 转置卷积（反卷积）
struct ggml_tensor * conv_t = ggml_conv_transpose_2d_p0(ctx, kernel, data, stride);
```

### 3.9 池化运算

```c
// 池化类型
enum ggml_op_pool {
    GGML_OP_POOL_MAX,  // 最大池化
    GGML_OP_POOL_AVG,  // 平均池化
};

// 1D 池化
struct ggml_tensor * pooled = ggml_pool_1d(ctx, x, GGML_OP_POOL_MAX, k0, s0, p0);

// 2D 池化
struct ggml_tensor * pooled = ggml_pool_2d(ctx, x, GGML_OP_POOL_MAX, k0, k1, s0, s1, p0, p1);
```

### 3.10 状态空间模型算子

```c
// SSM 卷积（用于 Mamba）
struct ggml_tensor * ssm_conv = ggml_ssm_conv(ctx, sx, c);

// SSM 扫描
struct ggml_tensor * ssm_scan = ggml_ssm_scan(ctx, s, x, dt, A, B, C, ids);
```

### 3.11 RWKV 算子

```c
// RWKV v6 注意力
struct ggml_tensor * wkv = ggml_rwkv_wkv6(ctx, k, v, r, tf, td, state);

// RWKV v7 注意力
struct ggml_tensor * wkv7 = ggml_rwkv_wkv7(ctx, r, w, k, v, a, b, state);

// 门控线性注意力
struct ggml_tensor * gla = ggml_gated_linear_attn(ctx, k, v, q, g, state, scale);
```

### 3.12 GLU（门控线性单元）

```c
enum ggml_glu_op {
    GGML_GLU_OP_REGLU,       // ReLU + GLU
    GGML_GLU_OP_GEGLU,       // GELU + GLU
    GGML_GLU_OP_SWIGLU,      // SiLU + GLU
    GGML_GLU_OP_SWIGLU_OAI,  // OpenAI 风格 SwiGLU
    GGML_GLU_OP_GEGLU_ERF,   // GELU (erf版本) + GLU
    GGML_GLU_OP_GEGLU_QUICK, // 快速 GELU + GLU
};

// GLU 操作
struct ggml_tensor * glu_out = ggml_glu(ctx, x, GGML_GLU_OP_SWIGLU, false);

// 分离式 GLU
struct ggml_tensor * swiglu = ggml_swiglu_split(ctx, gate, up);
```

### 3.13 优化器算子

```c
// AdamW 优化步骤
struct ggml_tensor * updated = ggml_opt_step_adamw(
    ctx,
    params,       // 参数张量
    grad,         // 梯度
    m,            // 一阶矩
    v,            // 二阶矩
    adamw_params  // [lr, beta1, beta2, eps, weight_decay, ...]
);

// SGD 优化步骤
struct ggml_tensor * updated = ggml_opt_step_sgd(
    ctx,
    params,
    grad,
    sgd_params    // [lr, weight_decay]
);
```

### 3.14 损失函数

```c
// 交叉熵损失
struct ggml_tensor * loss = ggml_cross_entropy_loss(ctx, logits, labels);
```

---

## 4. llama.cpp 中的算子使用

### 4.1 Transformer 层的典型实现

以下展示了 LLaMA 模型中一个 Transformer 层的计算流程：

```cpp
// 来自 src/models/llama.cpp

// 1. RMS Norm
cur = build_norm(inpL, model.layers[il].attn_norm, NULL, LLM_NORM_RMS, il);

// 2. 计算 Q, K, V
// Q = cur @ Wq^T
struct ggml_tensor * Qcur = ggml_mul_mat(ctx0, model.layers[il].wq, cur);
// K = cur @ Wk^T
struct ggml_tensor * Kcur = ggml_mul_mat(ctx0, model.layers[il].wk, cur);
// V = cur @ Wv^T
struct ggml_tensor * Vcur = ggml_mul_mat(ctx0, model.layers[il].wv, cur);

// 3. 应用 RoPE
Qcur = ggml_rope_ext(ctx0, Qcur, inp_pos, rope_factors, n_rot, rope_type, ...);
Kcur = ggml_rope_ext(ctx0, Kcur, inp_pos, rope_factors, n_rot, rope_type, ...);

// 4. 注意力计算
// 使用 Flash Attention
cur = ggml_flash_attn_ext(ctx0, Qcur, Kcur, Vcur, KQ_mask, kq_scale, 0.0f, 0.0f);

// 5. 输出投影
cur = ggml_mul_mat(ctx0, model.layers[il].wo, cur);

// 6. 残差连接
struct ggml_tensor * ffn_inp = ggml_add(ctx0, cur, inpSA);

// 7. FFN 前置归一化
cur = build_norm(ffn_inp, model.layers[il].ffn_norm, NULL, LLM_NORM_RMS, il);

// 8. FFN（SwiGLU）
// gate = cur @ W_gate^T
struct ggml_tensor * gate = ggml_mul_mat(ctx0, model.layers[il].ffn_gate, cur);
gate = ggml_silu(ctx0, gate);

// up = cur @ W_up^T
struct ggml_tensor * up = ggml_mul_mat(ctx0, model.layers[il].ffn_up, cur);

// down = (gate * up) @ W_down^T
cur = ggml_mul_mat(ctx0, model.layers[il].ffn_down, 
                   ggml_mul(ctx0, gate, up));

// 9. 残差连接
cur = ggml_add(ctx0, cur, ffn_inp);
```

### 4.2 注意力计算流程详解

```cpp
// 传统注意力实现方式

// 1. 计算 KQ 注意力分数
// KQ = Q @ K^T * scale
struct ggml_tensor * kq = ggml_mul_mat(ctx, K, Q);
kq = ggml_soft_max_ext(ctx, kq, KQ_mask, kq_scale, 0.0f);

// 2. 应用注意力到 V
// out = V @ KQ^T
struct ggml_tensor * kqv = ggml_mul_mat(ctx, V, kq);

// 3. 合并多头
struct ggml_tensor * kqv_merged = ggml_permute(ctx, kqv, 0, 2, 1, 3);
struct ggml_tensor * cur = ggml_cont_2d(ctx, kqv_merged, n_embd_head * n_head, n_tokens);
```

### 4.3 KV Cache 管理

```cpp
// 存储 K 到 KV cache
struct ggml_tensor * k_cache_view = ggml_view_1d(ctx, k_cache, n_tokens * n_embd_gqa,
    ggml_row_size(k_cache->type, n_embd_gqa) * kv_head);
ggml_cpy(ctx, Kcur, k_cache_view);

// 存储 V 到 KV cache
struct ggml_tensor * v_cache_view = ggml_view_2d(ctx, v_cache, n_tokens, n_embd_gqa,
    n_ctx * ggml_element_size(v_cache),
    kv_head * ggml_element_size(v_cache));
struct ggml_tensor * v_cur_t = ggml_transpose(ctx, ggml_reshape_2d(ctx, Vcur, n_embd_gqa, n_tokens));
ggml_cpy(ctx, v_cur_t, v_cache_view);
```

---

## 5. 后端架构

### 5.1 后端类型

GGML 支持多种计算后端：

| 后端 | 目录 | 说明 |
|------|------|------|
| CPU | `ggml-cpu/` | 通用 CPU 后端，支持 SIMD |
| CUDA | `ggml-cuda/` | NVIDIA GPU 后端 |
| Metal | `ggml-metal/` | Apple Metal 后端 |
| Vulkan | `ggml-vulkan/` | 跨平台 GPU 后端 |
| SYCL | `ggml-sycl/` | Intel GPU 后端 |
| OpenCL | `ggml-opencl/` | OpenCL 后端 |
| RPC | `ggml-rpc/` | 远程过程调用后端 |

### 5.2 后端接口

```c
// 后端设备类型
enum ggml_backend_dev_type {
    GGML_BACKEND_DEVICE_TYPE_CPU,   // CPU 设备
    GGML_BACKEND_DEVICE_TYPE_GPU,   // 独立 GPU
    GGML_BACKEND_DEVICE_TYPE_IGPU,  // 集成 GPU
    GGML_BACKEND_DEVICE_TYPE_ACCEL, // 加速器（BLAS/AMX）
};

// 创建后端
ggml_backend_t backend = ggml_backend_dev_init(device, params);

// 分配缓冲区
ggml_backend_buffer_t buffer = ggml_backend_alloc_buffer(backend, size);

// 执行计算图
ggml_backend_graph_compute(backend, cgraph);

// 异步执行
ggml_backend_graph_compute_async(backend, cgraph);
```

### 5.3 后端调度器

```c
// 创建调度器（支持多后端）
ggml_backend_sched_t sched = ggml_backend_sched_new(
    backends,     // 后端数组
    bufts,        // 缓冲区类型数组
    n_backends,   // 后端数量
    graph_size,   // 计算图大小
    parallel,     // 是否并行
    op_offload    // 是否允许算子卸载
);

// 分配并执行
ggml_backend_sched_alloc_graph(sched, graph);
ggml_backend_sched_graph_compute(sched, graph);

// 重置
ggml_backend_sched_reset(sched);
```

---

## 6. 量化类型

### 6.1 支持的量化格式

```c
enum ggml_type {
    // 基础浮点类型
    GGML_TYPE_F32,     // 32位浮点
    GGML_TYPE_F16,     // 16位浮点 (IEEE 754)
    GGML_TYPE_BF16,    // BFloat16

    // 整数类型
    GGML_TYPE_I8,      // 8位整数
    GGML_TYPE_I16,     // 16位整数
    GGML_TYPE_I32,     // 32位整数
    GGML_TYPE_I64,     // 64位整数
    GGML_TYPE_F64,     // 64位浮点

    // Q-quants (传统量化)
    GGML_TYPE_Q4_0,    // 4-bit, 32块, 缩放
    GGML_TYPE_Q4_1,    // 4-bit, 32块, 缩放+偏移
    GGML_TYPE_Q5_0,    // 5-bit
    GGML_TYPE_Q5_1,    // 5-bit
    GGML_TYPE_Q8_0,    // 8-bit, 32块, 缩放
    GGML_TYPE_Q8_1,    // 8-bit, 32块, 缩放+偏移

    // K-quants (优化量化)
    GGML_TYPE_Q2_K,    // 2-bit K-quant
    GGML_TYPE_Q3_K,    // 3-bit K-quant
    GGML_TYPE_Q4_K,    // 4-bit K-quant
    GGML_TYPE_Q5_K,    // 5-bit K-quant
    GGML_TYPE_Q6_K,    // 6-bit K-quant
    GGML_TYPE_Q8_K,    // 8-bit K-quant

    // I-quants (重要性矩阵量化)
    GGML_TYPE_IQ1_S,   // 1-bit I-quant
    GGML_TYPE_IQ1_M,   // 1-bit I-quant (中)
    GGML_TYPE_IQ2_XXS, // 2-bit I-quant (超小)
    GGML_TYPE_IQ2_XS,  // 2-bit I-quant (小)
    GGML_TYPE_IQ2_S,   // 2-bit I-quant
    GGML_TYPE_IQ3_XXS, // 3-bit I-quant
    GGML_TYPE_IQ3_S,   // 3-bit I-quant
    GGML_TYPE_IQ4_NL,  // 4-bit I-quant (标准)
    GGML_TYPE_IQ4_XS,  // 4-bit I-quant (小)

    // 新型量化格式
    GGML_TYPE_TQ1_0,   // Tiny 1-bit
    GGML_TYPE_TQ2_0,   // Tiny 2-bit
    GGML_TYPE_MXFP4,   // MXFP4
    GGML_TYPE_NVFP4,   // NVIDIA FP4
};
```

### 6.2 量化 API

```c
// 初始化量化表
ggml_quantize_init(GGML_TYPE_Q4_K);

// 量化数据
size_t quantized_size = ggml_quantize_chunk(
    GGML_TYPE_Q4_K,    // 目标类型
    src_float_data,    // 源浮点数据
    dst_buffer,        // 目标缓冲区
    start_row,         // 起始行
    n_rows,            // 行数
    n_per_row,         // 每行元素数
    importance_matrix  // 重要性矩阵（可选）
);

// 检查是否需要重要性矩阵
bool needs_imatrix = ggml_quantize_requires_imatrix(GGML_TYPE_IQ4_XS);
```

---

## 附录：完整算子列表

### 基础运算
| 算子 | 说明 |
|------|------|
| `GGML_OP_NONE` | 空操作 |
| `GGML_OP_DUP` | 复制张量 |
| `GGML_OP_ADD` | 加法 |
| `GGML_OP_ADD_ID` | 带索引的加法 |
| `GGML_OP_ADD1` | 加标量 |
| `GGML_OP_ACC` | 累加 |
| `GGML_OP_SUB` | 减法 |
| `GGML_OP_MUL` | 乘法 |
| `GGML_OP_DIV` | 除法 |
| `GGML_OP_SQR` | 平方 |
| `GGML_OP_SQRT` | 平方根 |
| `GGML_OP_LOG` | 对数 |
| `GGML_OP_SIN` | 正弦 |
| `GGML_OP_COS` | 余弦 |

### 聚合运算
| 算子 | 说明 |
|------|------|
| `GGML_OP_SUM` | 求和 |
| `GGML_OP_SUM_ROWS` | 按行求和 |
| `GGML_OP_CUMSUM` | 累积求和 |
| `GGML_OP_MEAN` | 均值 |
| `GGML_OP_ARGMAX` | 最大值索引 |
| `GGML_OP_COUNT_EQUAL` | 统计相等元素 |

### 归一化运算
| 算子 | 说明 |
|------|------|
| `GGML_OP_NORM` | 层归一化 |
| `GGML_OP_RMS_NORM` | RMS 归一化 |
| `GGML_OP_GROUP_NORM` | 组归一化 |
| `GGML_OP_L2_NORM` | L2 归一化 |

### 矩阵运算
| 算子 | 说明 |
|------|------|
| `GGML_OP_MUL_MAT` | 矩阵乘法 |
| `GGML_OP_MUL_MAT_ID` | 间接矩阵乘法 |
| `GGML_OP_OUT_PROD` | 外积 |

### 形状操作
| 算子 | 说明 |
|------|------|
| `GGML_OP_SCALE` | 缩放 |
| `GGML_OP_SET` | 设置值 |
| `GGML_OP_CPY` | 复制 |
| `GGML_OP_CONT` | 连续化 |
| `GGML_OP_RESHAPE` | 重塑 |
| `GGML_OP_VIEW` | 视图 |
| `GGML_OP_PERMUTE` | 维度重排 |
| `GGML_OP_TRANSPOSE` | 转置 |

### 注意力相关
| 算子 | 说明 |
|------|------|
| `GGML_OP_GET_ROWS` | 获取行 |
| `GGML_OP_SET_ROWS` | 设置行 |
| `GGML_OP_DIAG` | 对角矩阵 |
| `GGML_OP_DIAG_MASK_INF` | 对角掩码(-INF) |
| `GGML_OP_DIAG_MASK_ZERO` | 对角掩码(0) |
| `GGML_OP_SOFT_MAX` | Softmax |
| `GGML_OP_ROPE` | 旋转位置编码 |
| `GGML_OP_FLASH_ATTN_EXT` | Flash Attention |
| `GGML_OP_GATED_LINEAR_ATTN` | 门控线性注意力 |

### 卷积相关
| 算子 | 说明 |
|------|------|
| `GGML_OP_CONV_TRANSPOSE_1D` | 1D 转置卷积 |
| `GGML_OP_IM2COL` | 图像转列 |
| `GGML_OP_CONV_2D` | 2D 卷积 |
| `GGML_OP_CONV_3D` | 3D 卷积 |
| `GGML_OP_CONV_2D_DW` | 深度可分离卷积 |
| `GGML_OP_CONV_TRANSPOSE_2D` | 2D 转置卷积 |
| `GGML_OP_POOL_1D` | 1D 池化 |
| `GGML_OP_POOL_2D` | 2D 池化 |

### 高级运算
| 算子 | 说明 |
|------|------|
| `GGML_OP_UPSCALE` | 上采样 |
| `GGML_OP_PAD` | 填充 |
| `GGML_OP_ARANGE` | 范围数组 |
| `GGML_OP_TIMESTEP_EMBEDDING` | 时间步嵌入 |
| `GGML_OP_ARGSORT` | 排序索引 |
| `GGML_OP_TOP_K` | Top-K |
| `GGML_OP_LEAKY_RELU` | Leaky ReLU |
| `GGML_OP_TRI` | 三角矩阵 |
| `GGML_OP_FILL` | 填充 |

### SSM/RWKV 相关
| 算子 | 说明 |
|------|------|
| `GGML_OP_SSM_CONV` | SSM 卷积 |
| `GGML_OP_SSM_SCAN` | SSM 扫描 |
| `GGML_OP_RWKV_WKV6` | RWKV v6 |
| `GGML_OP_RWKV_WKV7` | RWKV v7 |
| `GGML_OP_GATED_DELTA_NET` | 门控 Delta 网络 |

### SAM 相关
| 算子 | 说明 |
|------|------|
| `GGML_OP_WIN_PART` | 窗口分割 |
| `GGML_OP_WIN_UNPART` | 窗口合并 |
| `GGML_OP_GET_REL_POS` | 获取相对位置 |
| `GGML_OP_ADD_REL_POS` | 添加相对位置 |

### 自定义运算
| 算子 | 说明 |
|------|------|
| `GGML_OP_UNARY` | 一元运算 |
| `GGML_OP_MAP_CUSTOM1` | 自定义运算1 |
| `GGML_OP_MAP_CUSTOM2` | 自定义运算2 |
| `GGML_OP_MAP_CUSTOM3` | 自定义运算3 |
| `GGML_OP_CUSTOM` | 完全自定义 |

### 优化与损失
| 算子 | 说明 |
|------|------|
| `GGML_OP_CROSS_ENTROPY_LOSS` | 交叉熵损失 |
| `GGML_OP_OPT_STEP_ADAMW` | AdamW 优化 |
| `GGML_OP_OPT_STEP_SGD` | SGD 优化 |
| `GGML_OP_GLU` | GLU 门控 |
