# llama.cpp 新模型支持指南

## 概述

当你获得一个新模型时，要让 llama.cpp 能够正确推理它，需要完成以下关键步骤：

```
┌─────────────────────────────────────────────────────────────────┐
│                    新模型支持完整流程                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐          │
│  │ 原始模型    │    │  GGUF 格式  │    │ llama.cpp   │          │
│  │ (PyTorch/   │───▶│  转换       │───▶│  推理执行   │          │
│  │  HuggingFace)│    │             │    │             │          │
│  └─────────────┘    └─────────────┘    └─────────────┘          │
│         │                  │                  │                  │
│         ▼                  ▼                  ▼                  │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐          │
│  │ 分析模型    │    │ 定义架构    │    │ 实现计算图  │          │
│  │ 结构与算子  │    │ 与张量映射  │    │ 与算子支持  │          │
│  └─────────────┘    └─────────────┘    └─────────────┘          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## 目录

1. [前提分析](#一前提分析)
2. [第一步：转换模型到 GGUF 格式](#二第一步转换模型到-gguf-格式)
3. [第二步：定义模型架构](#三第二步定义模型架构)
4. [第三步：实现 GGML 计算图](#四第三步实现-ggml-计算图)
5. [第四步：验证与测试](#五第四步验证与测试)
6. [常见问题与解决方案](#六常见问题与解决方案)

---

## 一、前提分析

在开始之前，你需要对新模型进行全面分析：

### 1.1 模型结构分析

**需要了解的关键信息：**

| 分析项 | 说明 | 如何获取 |
|--------|------|----------|
| 模型架构类型 | Transformer、Mamba、RWKV 等 | 查看 `config.json` 的 `architectures` 字段 |
| 层数、隐藏维度 | 决定模型规模 | `num_hidden_layers`, `hidden_size` |
| 注意力机制 | 是否使用 GQA、MQA、MLA 等 | `num_attention_heads`, `num_key_value_heads` |
| 位置编码 | RoPE、ALiBi、绝对位置编码等 | `rope_scaling`, `max_position_embeddings` |
| 激活函数 | GELU、SiLU、SwiGLU 等 | `hidden_act` |
| 特殊层结构 | MoE、SSM、滑动窗口等 | 查看模型代码实现 |

### 1.2 张量结构分析

```python
# 使用 Python 脚本分析模型张量
import torch
from transformers import AutoModel

model = AutoModel.from_pretrained("model_path", trust_remote_code=True)

# 打印所有张量名称和形状
for name, param in model.named_parameters():
    print(f"{name}: {param.shape}, dtype={param.dtype}")
```

### 1.3 判断是否已有架构支持

```bash
# 检查 llama.cpp 是否已支持该架构
grep -r "模型架构名" third_party/llama.cpp/src/
grep -r "MODEL_ARCH" third_party/llama.cpp/gguf-py/gguf/constants.py
```

---

## 二、第一步：转换模型到 GGUF 格式

### 2.1 GGUF 格式概述

GGUF (GGML Universal Format) 是 llama.cpp 使用的模型格式，包含：

```
┌─────────────────────────────────────────────────────────────────┐
│                      GGUF 文件结构                               │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    文件头 (Header)                       │    │
│  │  - GGUF Magic Number                                    │    │
│  │  - 版本号                                               │    │
│  │  - 张量数量、元数据数量                                 │    │
│  └─────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    元数据 (Metadata)                     │    │
│  │  - general.architecture: "llama"                        │    │
│  │  - llama.context_length: 4096                           │    │
│  │  - llama.embedding_length: 4096                         │    │
│  │  - llama.block_count: 32                                │    │
│  │  - tokenizer.model: "gpt2"                              │    │
│  │  - ...                                                  │    │
│  └─────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    张量数据 (Tensors)                    │    │
│  │  - token_embd.weight                                    │    │
│  │  - blk.0.attn_q.weight                                  │    │
│  │  - blk.0.attn_k.weight                                  │    │
│  │  - ...                                                  │    │
│  │  - output.weight                                        │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Python 端转换脚本开发

#### 2.2.1 定义模型架构枚举

**文件：`gguf-py/gguf/constants.py`**

```python
# 1. 添加架构枚举
class MODEL_ARCH(IntEnum):
    # ... 已有架构 ...
    MYMODEL = auto()  # 添加你的新模型

# 2. 定义架构名称映射
MODEL_ARCH_NAMES: dict[MODEL_ARCH, str] = {
    # ... 已有映射 ...
    MODEL_ARCH.MYMODEL: "mymodel",  # 架构标识符
}

# 3. 定义模型需要的张量类型
MODEL_TENSORS: dict[MODEL_ARCH, list[MODEL_TENSOR]] = {
    # ... 已有定义 ...
    MODEL_ARCH.MYMODEL: [
        MODEL_TENSOR.TOKEN_EMBD,      # Token Embedding
        MODEL_TENSOR.OUTPUT_NORM,     # 输出层归一化
        MODEL_TENSOR.OUTPUT,          # 输出层
        MODEL_TENSOR.ATTN_NORM,       # 注意力层归一化
        MODEL_TENSOR.ATTN_Q,          # Query 投影
        MODEL_TENSOR.ATTN_K,          # Key 投影
        MODEL_TENSOR.ATTN_V,          # Value 投影
        MODEL_TENSOR.ATTN_OUT,        # 注意力输出投影
        MODEL_TENSOR.FFN_NORM,        # FFN 层归一化
        MODEL_TENSOR.FFN_GATE,        # FFN Gate (SwiGLU)
        MODEL_TENSOR.FFN_DOWN,        # FFN Down 投影
        MODEL_TENSOR.FFN_UP,          # FFN Up 投影
    ],
}
```

#### 2.2.2 创建模型转换类

**文件：`convert_hf_to_gguf.py`**

```python
@ModelBase.register("MyModelForCausalLM")  # HuggingFace 模型类名
class MyModel(TextModel):
    model_arch = gguf.MODEL_ARCH.MYMODEL

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        # 从 config.json 读取超参数
        self.block_count = self.find_hparam(["num_hidden_layers", "n_layer"])
        
    def set_gguf_parameters(self):
        """设置 GGUF 元数据"""
        super().set_gguf_parameters()
        
        # 基本参数
        self.gguf_writer.add_context_length(
            self.find_hparam(["max_position_embeddings", "n_positions"])
        )
        self.gguf_writer.add_embedding_length(
            self.find_hparam(["hidden_size", "n_embd"])
        )
        self.gguf_writer.add_block_count(self.block_count)
        self.gguf_writer.add_feed_forward_length(
            self.find_hparam(["intermediate_size", "n_inner"])
        )
        
        # 注意力参数
        self.gguf_writer.add_head_count(
            self.find_hparam(["num_attention_heads", "n_head"])
        )
        self.gguf_writer.add_head_count_kv(
            self.find_hparam(["num_key_value_heads", "n_head_kv"])
        )
        
        # 层归一化参数
        self.gguf_writer.add_layer_norm_rms_eps(
            self.find_hparam(["rms_norm_eps", "layer_norm_epsilon"])
        )
        
        # RoPE 参数（如果使用）
        self.gguf_writer.add_rope_dimension_count(
            self.hparams.get("rope_dimension", 0)
        )
        self.gguf_writer.add_rope_freq_base(
            self.hparams.get("rope_theta", 10000.0)
        )

    def set_vocab(self):
        """设置词表"""
        # 方式 1: 使用 HuggingFace tokenizer
        self._set_vocab_sentencepiece()
        # 或: self._set_vocab_bpe()
        # 或: self._set_vocab_hf()

    def modify_tensors(self, data: Tensor, name: str, 
                       candidate_names: set[str]) -> Iterable[tuple[str, Tensor]]:
        """张量名称映射和修改"""
        # 张量名称映射到 GGUF 标准格式
        # 原始名称如: "model.layers.0.self_attn.q_proj.weight"
        # 映射为: "blk.0.attn_q.weight"
        
        mapped_name = self.tensor_map.get_name(name)
        if mapped_name is None:
            # 未映射的张量，跳过或警告
            print(f"Warning: unmapped tensor {name}")
            return []
            
        # 可能需要转置某些张量
        # PyTorch线性层权重: [out_features, in_features]
        # GGML期望: [in_features, out_features]
        if ".weight" in mapped_name and "norm" not in mapped_name:
            data = data.T
            
        return [(mapped_name, data)]
```

#### 2.2.3 张量名称映射

**文件：`gguf-py/gguf/tensor_mapping.py`**

```python
# 定义张量名称映射规则
block_mappings_cfg: dict[MODEL_TENSOR, tuple[str, ...]] = {
    # 注意力层 Query 投影的映射
    MODEL_TENSOR.ATTN_Q: (
        "model.layers.{bid}.self_attn.q_proj",      # LLaMA 风格
        "transformer.h.{bid}.attn.q_proj",          # GPT-2 风格
        "layers.{bid}.attention.q_proj",            # 其他风格
        # 添加你的模型张量名称模式
        "mymodel.layers.{bid}.attention.q",
    ),
    # 注意力层 Key 投影
    MODEL_TENSOR.ATTN_K: (
        "model.layers.{bid}.self_attn.k_proj",
        "transformer.h.{bid}.attn.k_proj",
        "mymodel.layers.{bid}.attention.k",
    ),
    # ... 其他张量映射
}
```

### 2.3 执行转换

```bash
# 基本转换命令
python convert_hf_to_gguf.py /path/to/model \
    --outfile /path/to/output.gguf \
    --outtype f16

# 转换为量化格式
python convert_hf_to_gguf.py /path/to/model \
    --outfile /path/to/output.gguf \
    --outtype q4_k_m

# 转换并设置名称
python convert_hf_to_gguf.py /path/to/model \
    --outfile /path/to/output.gguf \
    --model-name "my-model-v1"
```

---

## 三、第二步：定义模型架构

### 3.1 C++ 端架构定义

#### 3.1.1 添加架构枚举

**文件：`src/llama-arch.h`**

```cpp
enum llm_arch {
    // ... 已有架构 ...
    LLM_ARCH_MYMODEL,    // 添加你的新架构
    LLM_ARCH_UNKNOWN,
};
```

#### 3.1.2 添加架构名称映射

**文件：`src/llama-arch.cpp`**

```cpp
static const std::map<llm_arch, const char *> LLM_ARCH_NAMES = {
    // ... 已有映射 ...
    { LLM_ARCH_MYMODEL,    "mymodel"    },  // 与 Python 端一致
    { LLM_ARCH_UNKNOWN,    "(unknown)"  },
};
```

#### 3.1.3 定义张量布局

**文件：`src/llama-arch.cpp`**

```cpp
// 定义模型需要的张量
static const std::map<llm_arch, std::vector<llm_tensor>> LLM_TENSOR_NAMES = {
    // ... 已有定义 ...
    {
        LLM_ARCH_MYMODEL, {
            LLM_TENSOR_TOKEN_EMBD,
            LLM_TENSOR_OUTPUT_NORM,
            LLM_TENSOR_OUTPUT,
            LLM_TENSOR_ATTN_NORM,
            LLM_TENSOR_ATTN_Q,
            LLM_TENSOR_ATTN_K,
            LLM_TENSOR_ATTN_V,
            LLM_TENSOR_ATTN_OUT,
            LLM_TENSOR_FFN_NORM,
            LLM_TENSOR_FFN_GATE,
            LLM_TENSOR_FFN_DOWN,
            LLM_TENSOR_FFN_UP,
        }
    },
};
```

### 3.2 超参数加载

**文件：`src/llama-model.cpp`**

```cpp
void llama_model::load_hparams(llama_model_loader & ml) {
    // ... 已有代码 ...
    
    switch (arch) {
        // ... 已有架构处理 ...
        
        case LLM_ARCH_MYMODEL:
            {
                // 基本参数
                ml.get_key(LLM_KV_CONTEXT_LENGTH,    hparams.n_ctx);
                ml.get_key(LLM_KV_EMBEDDING_LENGTH,  hparams.n_embd);
                ml.get_key(LLM_KV_BLOCK_COUNT,       hparams.n_layer);
                ml.get_key(LLM_KV_FEED_FORWARD_LENGTH, hparams.n_ff);
                
                // 注意力参数
                ml.get_key(LLM_KV_ATTENTION_HEAD_COUNT,    hparams.n_head);
                ml.get_key(LLM_KV_ATTENTION_HEAD_COUNT_KV, hparams.n_head_kv);
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                
                // RoPE 参数（如果使用）
                ml.get_key(LLM_KV_ROPE_DIMENSION_COUNT, hparams.n_rot, false);
                ml.get_key(LLM_KV_ROPE_FREQ_BASE,       hparams.rope_freq_base, false);
                
                // 设置模型类型（用于日志）
                switch (hparams.n_layer) {
                    case 22: type = LLM_TYPE_1B; break;
                    case 32: type = LLM_TYPE_7B; break;
                    // ...
                    default: type = LLM_TYPE_UNKNOWN;
                }
            } break;
    }
}
```

### 3.3 RoPE 类型设置

**文件：`src/llama-model.cpp`**

```cpp
int32_t llama_model_rope_type(const llama_model * model) {
    switch (model->arch) {
        // ... 已有架构 ...
        
        case LLM_ARCH_MYMODEL:
            // 根据 RoPE 类型返回
            return LLAMA_ROPE_TYPE_NEOX;  // 大多数现代模型使用 NEOX
            
        default:
            return LLAMA_ROPE_TYPE_NONE;
    }
}
```

---

## 四、第三步：实现 GGML 计算图

### 4.1 计算图基础

GGML 计算图定义了模型的前向传播逻辑。每个算子对应一个 GGML 操作：

```
┌─────────────────────────────────────────────────────────────────┐
│                    计算图构建示例                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   输入 Token IDs                                                 │
│        │                                                         │
│        ▼                                                         │
│   ┌─────────┐                                                    │
│   │ Embedding│  ggml_get_rows()                                 │
│   └────┬────┘                                                    │
│        │                                                         │
│        ▼                                                         │
│   ┌─────────────────────────────────────────────┐               │
│   │              Transformer Block 0             │               │
│   │  ┌─────────────────────────────────────────┐│               │
│   │  │  Attention Layer                        ││               │
│   │  │  - RMSNorm                              ││               │
│   │  │  - Q/K/V Projection (ggml_mul_mat)      ││               │
│   │  │  - RoPE (ggml_rope)                     ││               │
│   │  │  - Attention Score (ggml_mul, ggml_soft ││               │
│   │  │  - Output Projection (ggml_mul_mat)     ││               │
│   │  └─────────────────────────────────────────┘│               │
│   │  ┌─────────────────────────────────────────┐│               │
│   │  │  FFN Layer                              ││               │
│   │  │  - RMSNorm                              ││               │
│   │  │  - Gate Projection (ggml_mul_mat)       ││               │
│   │  │  - SiLU Activation (ggml_silu)          ││               │
│   │  │  - Up/Down Projection (ggml_mul_mat)    ││               │
│   │  └─────────────────────────────────────────┘│               │
│   └─────────────────────────────────────────────┘               │
│        │                                                         │
│        ▼                                                         │
│   ... 更多 Transformer Blocks ...                               │
│        │                                                         │
│        ▼                                                         │
│   ┌─────────┐                                                    │
│   │Final Norm│  ggml_rms_norm()                                 │
│   └────┬────┘                                                    │
│        │                                                         │
│        ▼                                                         │
│   ┌─────────┐                                                    │
│   │  Output │  ggml_mul_mat()                                   │
│   └────┬────┘                                                    │
│        │                                                         │
│        ▼                                                         │
│   Logits (输出)                                                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 创建计算图实现

**文件：`src/models/mymodel.cpp`**

```cpp
#include "models.h"
#include "llama-model.h"

// 计算图上下文结构
struct llm_build_mymodel : public llm_graph_context {
    llm_build_mymodel(const llama_model & model, const llm_graph_params & params) 
        : llm_graph_context(params) {
        
        // 获取超参数
        const auto & hparams = model.hparams;
        const int n_layer = hparams.n_layer;
        
        // 获取输入
        struct ggml_tensor * cur = lctx.get_inp_tokens();  // 输入 token IDs
        struct ggml_tensor * inp_pos = lctx.get_inp_pos(); // 位置 IDs
        
        // ==================== Token Embedding ====================
        cur = ggml_get_rows(ctx0, model.tok_embd, cur);
        
        // ==================== Transformer Layers ====================
        for (int il = 0; il < n_layer; ++il) {
            // ---------- Self-Attention ----------
            
            // RMSNorm
            struct ggml_tensor * attn_norm = ggml_rms_norm(
                ctx0, cur, hparams.f_norm_rms_eps
            );
            attn_norm = ggml_mul(
                ctx0, attn_norm, 
                model.layers[il].attn_norm  // 可学习的缩放参数
            );
            
            // Q/K/V 投影
            struct ggml_tensor * Q = ggml_mul_mat(
                ctx0, model.layers[il].wq, attn_norm
            );
            struct ggml_tensor * K = ggml_mul_mat(
                ctx0, model.layers[il].wk, attn_norm
            );
            struct ggml_tensor * V = ggml_mul_mat(
                ctx0, model.layers[il].wv, attn_norm
            );
            
            // 应用 RoPE（旋转位置编码）
            Q = ggml_rope_ext(
                ctx0, Q, inp_pos, nullptr,
                hparams.n_rot, 0, hparams.rope_freq_base, 0.0f,
                1.0f, 0.0f, nullptr, ggml_rope_type::GGML_ROPE_TYPE_NEOX
            );
            K = ggml_rope_ext(
                ctx0, K, inp_pos, nullptr,
                hparams.n_rot, 0, hparams.rope_freq_base, 0.0f,
                1.0f, 0.0f, nullptr, ggml_rope_type::GGML_ROPE_TYPE_NEOX
            );
            
            // 注意力计算
            // Q * K^T / sqrt(d_k)
            struct ggml_tensor * KQ = ggml_mul_mat(
                ctx0, K, Q
            );
            KQ = ggml_scale(ctx0, KQ, 1.0f / sqrtf(hparams.n_embd / hparams.n_head));
            
            // Softmax
            KQ = ggml_soft_max(ctx0, KQ);
            
            // Attention * V
            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V, KQ);
            
            // 输出投影
            struct ggml_tensor * attn_out = ggml_mul_mat(
                ctx0, model.layers[il].wo, KQV
            );
            
            // 残差连接
            cur = ggml_add(ctx0, cur, attn_out);
            
            // ---------- Feed-Forward Network ----------
            
            // RMSNorm
            struct ggml_tensor * ffn_norm = ggml_rms_norm(
                ctx0, cur, hparams.f_norm_rms_eps
            );
            ffn_norm = ggml_mul(ctx0, ffn_norm, model.layers[il].ffn_norm);
            
            // SwiGLU 激活: gate * up
            struct ggml_tensor * gate = ggml_mul_mat(
                ctx0, model.layers[il].ffn_gate, ffn_norm
            );
            gate = ggml_silu(ctx0, gate);  // SiLU 激活
            
            struct ggml_tensor * up = ggml_mul_mat(
                ctx0, model.layers[il].ffn_up, ffn_norm
            );
            
            struct ggml_tensor * ffn_out = ggml_mul(ctx0, gate, up);
            
            // Down 投影
            ffn_out = ggml_mul_mat(ctx0, model.layers[il].ffn_down, ffn_out);
            
            // 残差连接
            cur = ggml_add(ctx0, cur, ffn_out);
        }
        
        // ==================== Output Layer ====================
        
        // 最终归一化
        cur = ggml_rms_norm(ctx0, cur, hparams.f_norm_rms_eps);
        cur = ggml_mul(ctx0, cur, model.output_norm);
        
        // 输出投影（得到 logits）
        cur = ggml_mul_mat(ctx0, model.output, cur);
        
        // 设置输出
        ggml_build_forward_expand(gf, cur);
    }
};
```

### 4.3 注册计算图

**文件：`src/llama-model.cpp`**

```cpp
std::unique_ptr<llm_graph_context> llama_model::build_graph(
    const llm_graph_params & params) {
    
    switch (arch) {
        // ... 已有架构 ...
        
        case LLM_ARCH_MYMODEL:
            return std::make_unique<llm_build_mymodel>(*this, params);
            
        default:
            throw std::runtime_error("unsupported architecture");
    }
}
```

### 4.4 常用 GGML 算子

| 算子 | 说明 | 使用场景 |
|------|------|----------|
| `ggml_mul_mat` | 矩阵乘法 | 线性层投影 |
| `ggml_mul` | 逐元素乘法 | 缩放、门控 |
| `ggml_add` | 逐元素加法 | 残差连接 |
| `ggml_rms_norm` | RMS 归一化 | 层归一化 |
| `ggml_layer_norm` | LayerNorm | 层归一化 |
| `ggml_silu` | SiLU 激活 | FFN 激活函数 |
| `ggml_gelu` | GELU 激活 | FFN 激活函数 |
| `ggml_soft_max` | Softmax | 注意力分数 |
| `ggml_rope_ext` | RoPE 编码 | 位置编码 |
| `ggml_get_rows` | 查表操作 | Embedding 查找 |
| `ggml_scale` | 缩放 | 注意力缩放 |
| `ggml_cpy` | 复制张量 | 类型转换 |
| `ggml_cont` | 连续化 | 内存布局优化 |

---

## 五、第四步：验证与测试

### 5.1 编译测试

```bash
# 编译 llama.cpp
mkdir build && cd build
cmake .. -DGGML_CUDA=ON  # 如果使用 CUDA
make -j$(nproc)

# 检查模型信息
./bin/llama-cli -m /path/to/model.gguf --verbose

# 测试推理
./bin/llama-cli -m /path/to/model.gguf \
    -p "Hello, how are you?" \
    -n 50 \
    --temp 0.7
```

### 5.2 对比验证

```python
# 使用 PyTorch 原始模型对比输出
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

model = AutoModelForCausalLM.from_pretrained("model_path")
tokenizer = AutoTokenizer.from_pretrained("model_path")

input_ids = tokenizer("Hello", return_tensors="pt").input_ids
with torch.no_grad():
    output = model(input_ids)
    logits = output.logits

print("PyTorch logits:", logits[0, -1, :10])  # 前10个token的logits
```

```bash
# 使用 llama.cpp 导出中间结果对比
./bin/llama-eval-callback -m /path/to/model.gguf \
    -p "Hello" \
    --log-disable
```

### 5.3 功能测试清单

| 测试项 | 命令 | 预期结果 |
|--------|------|----------|
| 基本推理 | `./llama-cli -m model.gguf -p "test" -n 10` | 正常输出文本 |
| 批处理 | `./llama-batched -m model.gguf` | 多序列并行处理 |
| 量化 | `./llama-quantize model.gguf model-q4.gguf Q4_K_M` | 成功量化 |
| Server | `./llama-server -m model.gguf --port 8080` | HTTP 服务正常 |
| Perplexity | `./llama-perplexity -m model.gguf -f test.txt` | PPL 值合理 |

---

## 六、常见问题与解决方案

### 6.1 张量形状不匹配

**问题**：`error: tensor 'blk.0.attn_q.weight' has wrong shape`

**解决方案**：
```python
# 检查是否需要转置
if "weight" in name and "norm" not in name:
    data = data.T  # 转置线性层权重
```

### 6.2 算子不支持

**问题**：`ggml backend does not support operation: XXX`

**解决方案**：
1. 检查是否可以在 CPU 上运行
2. 为 CUDA/Metal 后端实现该算子
3. 使用等价的已支持算子替代

### 6.3 KV Cache 大小问题

**问题**：`failed to allocate KV cache`

**解决方案**：
```bash
# 减小上下文长度
./llama-cli -m model.gguf -c 2048  # 默认可能是 4096
```

### 6.4 位置编码问题

**问题**：输出重复或无意义

**解决方案**：
1. 检查 RoPE 类型是否正确
2. 确认 `rope_freq_base` 参数
3. 验证位置索引是否正确传递

### 6.5 注意力掩码问题

**问题**：生成内容混乱

**解决方案**：
```cpp
// 确保使用因果掩码
KQ = ggml_soft_max_ext(ctx0, KQ, nullptr, 0.0f, 1);  // 设置 masked = true
```

---

## 七、完整示例：添加一个简单的 Transformer 模型

以下是一个完整的端到端示例：

### 7.1 Python 端

```python
# convert_hf_to_gguf.py

@ModelBase.register("SimpleTransformerForCausalLM")
class SimpleTransformerModel(TextModel):
    model_arch = gguf.MODEL_ARCH.SIMPLE_TRANSFORMER

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.block_count = self.find_hparam("n_layer")

    def set_gguf_parameters(self):
        super().set_gguf_parameters()
        self.gguf_writer.add_context_length(self.find_hparam("n_ctx"))
        self.gguf_writer.add_embedding_length(self.find_hparam("d_model"))
        self.gguf_writer.add_block_count(self.block_count)
        self.gguf_writer.add_head_count(self.find_hparam("n_head"))
        self.gguf_writer.add_layer_norm_rms_eps(1e-5)
        self.gguf_writer.add_rope_freq_base(10000.0)

    def modify_tensors(self, data, name, candidate_names):
        name = name.replace("transformer.", "")
        name = name.replace(".weight", "")
        
        mappings = {
            "wte": "token_embd",
            "ln_f": "output_norm",
            "lm_head": "output",
        }
        
        for old, new in mappings.items():
            name = name.replace(old, new)
        
        # 处理层张量
        import re
        match = re.match(r"layers\.(\d+)\.(.+)", name)
        if match:
            layer_idx = match.group(1)
            rest = match.group(2)
            name = f"blk.{layer_idx}.{rest}"
        
        # 转置线性层权重
        if any(x in name for x in ["q_proj", "k_proj", "v_proj", "out_proj", "fc"]):
            data = data.T
        
        return [(name + ".weight", data)]
```

### 7.2 C++ 端

```cpp
// src/llama-arch.h
enum llm_arch {
    // ...
    LLM_ARCH_SIMPLE_TRANSFORMER,
    LLM_ARCH_UNKNOWN,
};

// src/llama-arch.cpp
static const std::map<llm_arch, const char *> LLM_ARCH_NAMES = {
    // ...
    { LLM_ARCH_SIMPLE_TRANSFORMER, "simple-transformer" },
};

// src/models/simple_transformer.cpp
struct llm_build_simple_transformer : public llm_graph_context {
    llm_build_simple_transformer(const llama_model & model, 
                                   const llm_graph_params & params) 
        : llm_graph_context(params) {
        
        auto * cur = lctx.get_inp_tokens();
        auto * pos = lctx.get_inp_pos();
        
        // Embedding
        cur = ggml_get_rows(ctx0, model.tok_embd, cur);
        
        // Layers
        for (int il = 0; il < model.hparams.n_layer; il++) {
            auto * norm = ggml_rms_norm(ctx0, cur, 1e-5f);
            norm = ggml_mul(ctx0, norm, model.layers[il].attn_norm);
            
            auto * q = ggml_mul_mat(ctx0, model.layers[il].wq, norm);
            auto * k = ggml_mul_mat(ctx0, model.layers[il].wk, norm);
            auto * v = ggml_mul_mat(ctx0, model.layers[il].wv, norm);
            
            q = ggml_rope_ext(ctx0, q, pos, nullptr, 
                             model.hparams.n_rot, 0, 10000.0f, 0.0f, 1.0f, 0.0f, nullptr,
                             GGML_ROPE_TYPE_NEOX);
            k = ggml_rope_ext(ctx0, k, pos, nullptr,
                             model.hparams.n_rot, 0, 10000.0f, 0.0f, 1.0f, 0.0f, nullptr,
                             GGML_ROPE_TYPE_NEOX);
            
            auto * kq = ggml_mul_mat(ctx0, k, q);
            kq = ggml_soft_max(ctx0, kq);
            auto * kqv = ggml_mul_mat(ctx0, v, kq);
            auto * attn_out = ggml_mul_mat(ctx0, model.layers[il].wo, kqv);
            
            cur = ggml_add(ctx0, cur, attn_out);
            
            // FFN
            auto * ffn_norm = ggml_rms_norm(ctx0, cur, 1e-5f);
            ffn_norm = ggml_mul(ctx0, ffn_norm, model.layers[il].ffn_norm);
            
            auto * gate = ggml_silu(ctx0, 
                ggml_mul_mat(ctx0, model.layers[il].ffn_gate, ffn_norm));
            auto * up = ggml_mul_mat(ctx0, model.layers[il].ffn_up, ffn_norm);
            auto * ffn_out = ggml_mul_mat(ctx0, model.layers[il].ffn_down,
                ggml_mul(ctx0, gate, up));
            
            cur = ggml_add(ctx0, cur, ffn_out);
        }
        
        // Output
        cur = ggml_rms_norm(ctx0, cur, 1e-5f);
        cur = ggml_mul(ctx0, cur, model.output_norm);
        cur = ggml_mul_mat(ctx0, model.output, cur);
        
        ggml_build_forward_expand(gf, cur);
    }
};
```

---

## 八、参考资料

### 8.1 官方文档

- [GGUF 格式规范](https://github.com/ggml-org/ggml/blob/master/docs/gguf.md)
- [HOWTO-add-model.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/development/HOWTO-add-model.md)

### 8.2 相关 PR 参考

| PR | 内容 |
|-----|------|
| [#2268](https://github.com/ggml-org/llama.cpp/pull/2268) | YaRN RoPE scaling |
| [#3009](https://github.com/ggml-org/llama.cpp/pull/3009) | Baichuan 模型支持 |
| [#4406](https://github.com/ggml-org/llama.cpp/pull/4406) | Mixtral MoE 支持 |
| [#5423](https://github.com/ggml-org/llama.cpp/pull/5423) | BERT embeddings |
| [#6204](https://github.com/ggml-org/llama.cpp/pull/6204) | Grok-1 支持 |
| [#6515](https://github.com/ggml-org/llama.cpp/pull/6515) | DBRX 支持 |

### 8.3 调试工具

- `llama-eval-callback`: 打印每层计算的中间结果
- `llama-perplexity`: 计算困惑度验证模型质量
- `gguf-dump`: 查看 GGUF 文件内容

```bash
# 查看 GGUF 文件元数据
python -c "import gguf; gguf.GGUFReader('model.gguf').print_kv()"
```
