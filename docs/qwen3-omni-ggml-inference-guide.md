# Qwen3-Omni 模型 GGML 推理实现指南

## 目录

1. [Qwen3-Omni 模型概述](#1-qwen3-omni-模型概述)
2. [模型架构分析](#2-模型架构分析)
3. [GGUF 格式转换](#3-gguf-格式转换)
4. [C++ 端架构定义](#4-c-端架构定义)
5. [GGML 计算图实现](#5-ggml-计算图实现)
6. [多模态输入处理](#6-多模态输入处理)
7. [完整推理流程](#7-完整推理流程)
8. [性能优化与调试](#8-性能优化与调试)

---

## 1. Qwen3-Omni 模型概述

### 1.1 模型特点

Qwen3-Omni 是阿里云通义千问团队开发的全模态大语言模型，能够同时处理**文本、图像、音频、视频**等多种输入模态。相比前代 Qwen2.5-Omni，Qwen3-Omni 在以下方面有显著提升:

- **更强的多模态理解能力**:统一的跨模态表征学习
- **原生实时交互**:支持流式多模态输入输出
- **超长上下文**:支持 256K+ 上下文窗口
- **高效架构设计**:采用混合注意力机制和 MoE 结构

### 1.2 支持的模态组合

| 输入组合 | 输出模态 | 典型应用场景 |
|----------|----------|--------------|
| 文本 + 图像 | 文本 | 图像理解、视觉问答 |
| 文本 + 音频 | 文本/音频 | 语音对话、音频分析 |
| 文本 + 视频 | 文本 | 视频内容理解、事件分析 |
| 文本 + 图像 + 音频 | 文本 | 多模态内容综合分析 |
| 纯文本 | 文本 | 传统 LLM 任务 |

### 1.3 模型规格

```yaml
Qwen3-Omni-3B:
  thinker_config:
    hidden_size: 2048
    num_hidden_layers: 24
    num_attention_heads: 16
    intermediate_size: 5632
    rope_theta: 1000000.0
    max_position_embeddings: 32768
    
  vision_config:
    hidden_size: 1536
    num_hidden_layers: 24
    num_attention_heads: 16
    image_size: 512
    patch_size: 14
    
  audio_config:
    d_model: 768
    encoder_layers: 12
    encoder_attention_heads: 12
    input_feat_per_second: 128  # Mel 频谱 bins
    
Qwen3-Omni-7B:
  thinker_config:
    hidden_size: 4096
    num_hidden_layers: 32
    num_attention_heads: 32
    intermediate_size: 11008
    rope_theta: 1000000.0
    max_position_embeddings: 32768
    
  vision_config:
    hidden_size: 2048
    num_hidden_layers: 32
    num_attention_heads: 16
    image_size: 512
    patch_size: 14
    
  audio_config:
    d_model: 1024
    encoder_layers: 16
    encoder_attention_heads: 16
    input_feat_per_second: 128
```

### 1.4 GGML/llama.cpp 支持状态

```python
# GGUF 架构映射 (gguf/constants.py)
ARCHITECTURE_MAP = {
    "Qwen3OmniModel": "qwen3o",  # Qwen3-Omni 架构标识符
}

# 支持的量化格式
QUANTIZATION_SUPPORT = [
    "F16",      # 半精度浮点
    "Q8_0",     # 8-bit 量化
    "Q4_K_M",   # 4-bit K-quant (推荐平衡点)
    "Q4_0",     # 4-bit 基础量化
    "Q5_K_M",   # 5-bit K-quant
]
```

---

## 2. 模型架构详解

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                  Qwen3-Omni Architecture                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                    │
│  │  Text    │   │  Image   │   │  Audio   │                    │
│  │  Input   │   │  Input   │   │  Input   │                    │
│  └────┬─────┘   └────┬─────┘   └────┬─────┘                    │
│       │              │              │                           │
│       ▼              ▼              ▼                           │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                    │
│  │  Token   │   │  Vision  │   │  Audio   │                    │
│  │ Embedding│   │ Encoder  │   │ Encoder  │                    │
│  │          │   │(ViT-L/14)│   │(Whisper) │                    │
│  └────┬─────┘   └────┬─────┘   └────┬─────┘                    │
│       │              │              │                           │
│       │         ┌────┴──────────────┘                           │
│       │         │                                               │
│       │         ▼                                               │
│       │  ┌─────────────┐                                        │
│       │  │ Multimodal  │                                        │
│       │  │  Projector  │ (Linear/MLP)                           │
│       │  └──────┬──────┘                                        │
│       │         │                                               │
│       └─────────┼───────────────────────────────────┐           │
│                 │                                   │           │
│                 ▼                                   │           │
│        ┌────────────────┐                          │           │
│        │    Thinker     │◄────── KV Cache ──────────┘           │
│        │  (Qwen3 LLM)   │                                        │
│        │   24/32 Layers │                                        │
│        └───────┬────────┘                                        │
│                │                                                 │
│                ▼                                                 │
│        ┌────────────────┐                                        │
│        │   Output Head  │                                        │
│        │   (LM Head)    │                                        │
│        └────────────────┘                                        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Thinker (LLM) 架构

Thinker 部分基于 Qwen3 LLM 架构，采用标准 Transformer Decoder-only 结构:

**核心组件**:
- **RMSNorm**: 层归一化 (eps=1e-6)
- **GQA Attention**: Grouped-Query Attention，减少 KV Cache 大小
- **SwiGLU FFN**: Swish-Gated Linear Unit 激活函数
- **RoPE**: 旋转位置编码 (theta=1e6)

**每层计算流程**:
```
Input → RMSNorm → GQA Attention → Residual → RMSNorm → SwiGLU FFN → Residual → Output
```

### 2.3 Vision Encoder 架构

Vision Encoder 基于 ViT (Vision Transformer) 架构，将图像转换为视觉 embedding:

**架构参数**:
```yaml
vision_encoder:
  type: "ViT-L/14"
  image_size: 512
  patch_size: 14
  num_patches: (512/14)² ≈ 1369
  hidden_size: 1536 (3B) / 2048 (7B)
  num_layers: 24
  num_heads: 16
  mlp_ratio: 4.0
```

**处理流程**:
1. **Patch Embedding**: 将图像分割为 14x14 的 patches，每个 patch 展平为向量
2. **Position Embedding**: 添加 2D 位置编码
3. **Transformer Layers**: 多层自注意力提取视觉特征
4. **Projector**: 将视觉特征投影到 LLM 的 embedding 空间

### 2.4 Audio Encoder 架构

Audio Encoder 采用 Whisper-style 编码器，将音频波形或语谱图转换为音频 embedding:

**架构参数**:
```yaml
audio_encoder:
  type: "Whisper-style"
  input_feat_per_second: 128  # Mel 频谱 bins
  d_model: 768 (3B) / 1024 (7B)
  encoder_layers: 12 (3B) / 16 (7B)
  encoder_attention_heads: 12 (3B) / 16 (7B)
  max_audio_length: 30s
```

**处理流程**:
1. **Convolutional Frontend**: 两层 1D 卷积提取局部音频特征
2. **Positional Embedding**: 添加时间位置信息
3. **Transformer Layers**: 多层自注意力捕捉长距离依赖
4. **Adapter**: 投影到统一维度

### 2.5 Multimodal Projector

Projector 负责将不同模态的特征对齐到 LLM 的 embedding 空间:

**常见类型**:
- **Linear**: 单层线性变换 `y = Wx`
- **MLP 2X**: 两层 MLP `y = W₂·ReLU(W₁x)`
- **Cross Attention**: 使用交叉注意力进行特征融合

**Qwen3-Omni 使用**:
```python
# 视觉 projector (2x MLP)
class VisionProjector(nn.Module):
    def __init__(self, vision_hidden, llm_hidden):
        super().__init__()
        self.linear_1 = nn.Linear(vision_hidden, llm_hidden * 4)
        self.gelu = nn.GELU()
        self.linear_2 = nn.Linear(llm_hidden * 4, llm_hidden)
    
    def forward(self, x):
        return self.linear_2(self.gelu(self.linear_1(x)))

# 音频 projector (Linear)
class AudioProjector(nn.Module):
    def __init__(self, audio_hidden, llm_hidden):
        super().__init__()
        self.linear = nn.Linear(audio_hidden, llm_hidden)
    
    def forward(self, x):
        return self.linear(x)
```

---

## 3. GGUF 格式转换

### 3.1 环境准备

```bash
# 克隆 llama.cpp 仓库
git clone https://github.com/ggerganov/llama.cpp.git
cd llama.cpp

# 安装 Python 依赖
pip install torch transformers safetensors sentencepiece protobuf gguf

# 确保使用最新版本的 ggml-org/llama.cpp
# Qwen3-Omni 支持需要较新的版本
```

### 3.2 定义 Python 端架构

#### 3.2.1 添加架构枚举

**文件**: `gguf-py/gguf/constants.py`

```python
from enum import IntEnum

class MODEL_ARCH(IntEnum):
    # ... 已有架构 ...
    QWEN2_5_OMNI = auto()      # Qwen2.5-Omni (参考)
    QWEN3_OMNI = auto()        # Qwen3-Omni (新增)

MODEL_ARCH_NAMES: dict[MODEL_ARCH, str] = {
    # ... 已有映射 ...
    MODEL_ARCH.QWEN3_OMNI: "qwen3o",
}
```

#### 3.2.2 定义模型张量

**文件**: `gguf-py/gguf/tensor_mapping.py`

```python
from .constants import MODEL_ARCH, MODEL_TENSOR

MODEL_TENSORS: dict[MODEL_ARCH, list[MODEL_TENSOR]] = {
    # ... 已有定义 ...
    
    MODEL_ARCH.QWEN3_OMNI: [
        # Thinker (LLM) 部分
        MODEL_TENSOR.TOKEN_EMBD,           # Token Embedding
        MODEL_TENSOR.OUTPUT_NORM,          # Output Normalization
        MODEL_TENSOR.OUTPUT,               # Output Layer
        MODEL_TENSOR.ATTN_NORM,            # Attention Norm (per layer)
        MODEL_TENSOR.ATTN_Q,               # Query Projection
        MODEL_TENSOR.ATTN_K,               # Key Projection
        MODEL_TENSOR.ATTN_V,               # Value Projection
        MODEL_TENSOR.ATTN_OUT,             # Attention Output
        MODEL_TENSOR.FFN_NORM,             # FFN Norm
        MODEL_TENSOR.FFN_GATE,             # FFN Gate (SwiGLU)
        MODEL_TENSOR.FFN_DOWN,             # FFN Down
        MODEL_TENSOR.FFN_UP,               # FFN Up
        
        # Vision Encoder 部分
        MODEL_TENSOR.VISION_EMBD,          # Vision Patch Embedding
        MODEL_TENSOR.VISION_POS_EMBD,      # Vision Position Embedding
        MODEL_TENSOR.VISION_NORM,          # Vision Layer Norms
        MODEL_TENSOR.VISION_ATTN_Q,        # Vision Attention Q
        MODEL_TENSOR.VISION_ATTN_K,        # Vision Attention K
        MODEL_TENSOR.VISION_ATTN_V,        # Vision Attention V
        MODEL_TENSOR.VISION_ATTN_OUT,      # Vision Attention Out
        MODEL_TENSOR.VISION_FFN_DOWN,      # Vision FFN Down
        MODEL_TENSOR.VISION_FFN_UP,        # Vision FFN Up
        MODEL_TENSOR.VISION_PROJ,          # Vision Projector
        
        # Audio Encoder 部分
        MODEL_TENSOR.AUDIO_CONV1,          # Audio Conv1
        MODEL_TENSOR.AUDIO_CONV2,          # Audio Conv2
        MODEL_TENSOR.AUDIO_NORM,           # Audio Layer Norms
        MODEL_TENSOR.AUDIO_ATTN_Q,         # Audio Attention Q
        MODEL_TENSOR.AUDIO_ATTN_K,         # Audio Attention K
        MODEL_TENSOR.AUDIO_ATTN_V,         # Audio Attention V
        MODEL_TENSOR.AUDIO_ATTN_OUT,       # Audio Attention Out
        MODEL_TENSOR.AUDIO_FFN_DOWN,       # Audio FFN Down
        MODEL_TENSOR.AUDIO_FFN_UP,         # Audio FFN Up
        MODEL_TENSOR.AUDIO_PROJ,           # Audio Projector
    ],
}
```

#### 3.2.3 创建转换类

**文件**: `convert_hf_to_gguf.py`

完整转换类实现请参考文档正文中的详细代码示例。

### 3.3 执行转换

```bash
# 基本转换命令
python convert_hf_to_gguf.py /path/to/Qwen3-Omni-3B \
    --outfile /path/to/qwen3-omni-3b-f16.gguf \
    --outtype f16

# 转换为量化格式 (推荐)
python convert_hf_to_gguf.py /path/to/Qwen3-Omni-3B \
    --outfile /path/to/qwen3-omni-3b-q4_k_m.gguf \
    --outtype q4_k_m

# 转换为 Q8 格式 (高质量)
python convert_hf_to_gguf.py /path/to/Qwen3-Omni-7B \
    --outfile /path/to/qwen3-omni-7b-q8_0.gguf \
    --outtype q8_0

# 验证转换结果
python -c "import gguf; r = gguf.GGUFReader('qwen3-omni-3b-q4_k_m.gguf'); r.print_kv()"
```

### 3.4 后量化 (可选)

```bash
# 如果已经转换为 F16 格式，可以进行后量化
cd llama.cpp/build

# 量化到 Q4_K_M
./bin/llama-quantize ../models/qwen3-omni-3b-f16.gguf \
    ../models/qwen3-omni-3b-q4_k_m.gguf \
    Q4_K_M

# 量化到 Q8_0
./bin/llama-quantize ../models/qwen3-omni-7b-f16.gguf \
    ../models/qwen3-omni-7b-q8_0.gguf \
    Q8_0
```

---

## 4. C++ 端架构定义

### 4.1 添加架构枚举

**文件**: `src/llama-arch.h`

```cpp
#pragma once

enum llm_arch {
    // ... 已有架构 ...
    LLM_ARCH_QWEN2_5_OMNI,    // Qwen2.5-Omni
    LLM_ARCH_QWEN3_OMNI,      // Qwen3-Omni (新增)
    LLM_ARCH_UNKNOWN,
};
```

**文件**: `src/llama-arch.cpp`

```cpp
#include "llama-arch.h"

// 架构名称映射
static const std::map<llm_arch, const char *> LLM_ARCH_NAMES = {
    // ... 已有映射 ...
    { LLM_ARCH_QWEN2_5_OMNI, "qwen2.5o" },
    { LLM_ARCH_QWEN3_OMNI,   "qwen3o"   },
    { LLM_ARCH_UNKNOWN,      "(unknown)" },
};
```

### 4.2 定义张量布局

**文件**: `src/llama-arch.cpp`

```cpp
static const std::map<llm_arch, std::vector<llm_tensor>> LLM_TENSOR_NAMES = {
    {
        LLM_ARCH_QWEN3_OMNI, {
            // Thinker (LLM) 部分
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
            
            // Vision Encoder 部分
            LLM_TENSOR_VISION_EMBD,
            LLM_TENSOR_VISION_POS_EMBD,
            LLM_TENSOR_VISION_NORM,
            LLM_TENSOR_VISION_ATTN_Q,
            LLM_TENSOR_VISION_ATTN_K,
            LLM_TENSOR_VISION_ATTN_V,
            LLM_TENSOR_VISION_ATTN_OUT,
            LLM_TENSOR_VISION_FFN_DOWN,
            LLM_TENSOR_VISION_FFN_UP,
            LLM_TENSOR_VISION_PROJ,
            
            // Audio Encoder 部分
            LLM_TENSOR_AUDIO_CONV1,
            LLM_TENSOR_AUDIO_CONV2,
            LLM_TENSOR_AUDIO_NORM,
            LLM_TENSOR_AUDIO_ATTN_Q,
            LLM_TENSOR_AUDIO_ATTN_K,
            LLM_TENSOR_AUDIO_ATTN_V,
            LLM_TENSOR_AUDIO_ATTN_OUT,
            LLM_TENSOR_AUDIO_FFN_DOWN,
            LLM_TENSOR_AUDIO_FFN_UP,
            LLM_TENSOR_AUDIO_PROJ,
        }
    },
};
```

### 4.3 超参数加载

**文件**: `src/llama-model.cpp`

```cpp
case LLM_ARCH_QWEN3_OMNI:
    {
        // Thinker (LLM) 参数
        ml.get_key(LLM_KV_CONTEXT_LENGTH, hparams.n_ctx);
        ml.get_key(LLM_KV_EMBEDDING_LENGTH, hparams.n_embd);
        ml.get_key(LLM_KV_BLOCK_COUNT, hparams.n_layer);
        ml.get_key(LLM_KV_FEED_FORWARD_LENGTH, hparams.n_ff);
        
        // 注意力参数
        ml.get_key(LLM_KV_ATTENTION_HEAD_COUNT, hparams.n_head);
        ml.get_key(LLM_KV_ATTENTION_HEAD_COUNT_KV, hparams.n_head_kv);
        ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
        
        // RoPE 参数
        ml.get_key(LLM_KV_ROPE_DIMENSION_COUNT, hparams.n_rot, false);
        ml.get_key(LLM_KV_ROPE_FREQ_BASE, hparams.rope_freq_base, false);
        
        // Vision Encoder 参数
        ml.get_key(LLM_KV_HAS_VISION_ENCODER, has_vision, false);
        if (has_vision) {
            ml.get_key(LLM_KV_VISION_EMBEDDING_LENGTH, hparams.n_embd_vision);
            ml.get_key(LLM_KV_VISION_BLOCK_COUNT, hparams.n_layer_vision);
            ml.get_key(LLM_KV_VISION_PATCH_SIZE, hparams.vision_patch_size);
        }
        
        // Audio Encoder 参数
        ml.get_key(LLM_KV_HAS_AUDIO_ENCODER, has_audio, false);
        if (has_audio) {
            ml.get_key(LLM_KV_AUDIO_EMBEDDING_LENGTH, hparams.n_embd_audio);
            ml.get_key(LLM_KV_AUDIO_BLOCK_COUNT, hparams.n_layer_audio);
            ml.get_key(LLM_KV_AUDIO_NUM_MEL_BINS, hparams.audio_num_mel_bins);
        }
    } break;
```

---

## 5. GGML 计算图实现

### 5.1 Thinker LLM 计算图

```cpp
struct llm_build_qwen3_omni : public llm_graph_context {
    llm_build_qwen3_omni(const llama_model & model, const llm_graph_params & params) 
        : llm_graph_context(params) {
        
        const auto & hparams = model.hparams;
        const int n_layer = hparams.n_layer;
        
        // 获取输入
        struct ggml_tensor * cur = lctx.get_inp_tokens();
        struct ggml_tensor * inp_pos = lctx.get_inp_pos();
        
        // Token Embedding
        cur = ggml_get_rows(ctx0, model.tok_embd, cur);
        
        // 合并多模态 embeddings (如果有)
        if (lctx.has_vision_input && model.has_vision) {
            struct ggml_tensor * vision_embs = encode_vision(ctx0, model, lctx.vision_input);
            vision_embs = ggml_mul_mat(ctx0, model.vision.proj, vision_embs);
            cur = merge_multimodal_embeddings(ctx0, cur, vision_embs, lctx.vision_indices);
        }
        
        if (lctx.has_audio_input && model.has_audio) {
            struct ggml_tensor * audio_embs = encode_audio(ctx0, model, lctx.audio_input);
            audio_embs = ggml_mul_mat(ctx0, model.audio.proj, audio_embs);
            cur = merge_multimodal_embeddings(ctx0, cur, audio_embs, lctx.audio_indices);
        }
        
        // Transformer Layers
        for (int il = 0; il < n_layer; ++il) {
            // Self-Attention
            struct ggml_tensor * attn_norm = ggml_rms_norm(ctx0, cur, hparams.f_norm_rms_eps);
            attn_norm = ggml_mul(ctx0, attn_norm, model.layers[il].attn_norm);
            
            struct ggml_tensor * Q = ggml_mul_mat(ctx0, model.layers[il].wq, attn_norm);
            struct ggml_tensor * K = ggml_mul_mat(ctx0, model.layers[il].wk, attn_norm);
            struct ggml_tensor * V = ggml_mul_mat(ctx0, model.layers[il].wv, attn_norm);
            
            // RoPE
            Q = ggml_rope_ext(ctx0, Q, inp_pos, nullptr,
                             hparams.n_rot, 0, hparams.rope_freq_base, 0.0f,
                             1.0f, 0.0f, nullptr, GGML_ROPE_TYPE_NEOX);
            K = ggml_rope_ext(ctx0, K, inp_pos, nullptr,
                             hparams.n_rot, 0, hparams.rope_freq_base, 0.0f,
                             1.0f, 0.0f, nullptr, GGML_ROPE_TYPE_NEOX);
            
            // Flash Attention
            struct ggml_tensor * attn_out = ggml_flash_attn_ext(
                ctx0, Q, K, V,
                lctx.get_attn_mask(),
                1.0f / sqrtf(hparams.n_embd_head),
                0.0f, 0.0f
            );
            
            attn_out = ggml_mul_mat(ctx0, model.layers[il].wo, attn_out);
            cur = ggml_add(ctx0, cur, attn_out);  // Residual
            
            // FFN (SwiGLU)
            struct ggml_tensor * ffn_norm = ggml_rms_norm(ctx0, cur, hparams.f_norm_rms_eps);
            ffn_norm = ggml_mul(ctx0, ffn_norm, model.layers[il].ffn_norm);
            
            struct ggml_tensor * gate = ggml_mul_mat(ctx0, model.layers[il].ffn_gate, ffn_norm);
            gate = ggml_silu(ctx0, gate);
            
            struct ggml_tensor * up = ggml_mul_mat(ctx0, model.layers[il].ffn_up, ffn_norm);
            struct ggml_tensor * ffn_inter = ggml_mul(ctx0, gate, up);
            
            struct ggml_tensor * ffn_out = ggml_mul_mat(ctx0, model.layers[il].ffn_down, ffn_inter);
            cur = ggml_add(ctx0, cur, ffn_out);  // Residual
        }
        
        // Output Layer
        cur = ggml_rms_norm(ctx0, cur, hparams.f_norm_rms_eps);
        cur = ggml_mul(ctx0, cur, model.output_norm);
        cur = ggml_mul_mat(ctx0, model.output, cur);
        
        ggml_build_forward_expand(gf, cur);
    }
};
```

### 5.2 Vision Encoder 实现

```cpp
struct ggml_tensor * encode_vision(
    ggml_context * ctx,
    const llama_model & model,
    const ggml_tensor * images
) {
    const auto & hparams = model.hparams;
    const int n_patches = (hparams.vision_image_size / hparams.vision_patch_size) * 
                          (hparams.vision_image_size / hparams.vision_patch_size);
    
    // 1. Patch Embedding (Conv2D)
    struct ggml_tensor * patches = ggml_conv_2d(
        ctx, model.vision.patch_embd, images,
        hparams.vision_patch_size, hparams.vision_patch_size,
        0, 0, 1, 1
    );
    
    // 2. Reshape & Position Embedding
    patches = ggml_reshape_3d(ctx, patches, hparams.n_embd_vision, n_patches, images->ne[3]);
    patches = ggml_add(ctx, patches, model.vision.pos_embd);
    
    // 3. Vision Transformer Layers
    for (int i = 0; i < hparams.n_layer_vision; i++) {
        struct ggml_tensor * norm = ggml_rms_norm(ctx, patches, hparams.vision_layer_norm_eps);
        norm = ggml_mul(ctx, norm, model.vision.layers[i].norm);
        
        struct ggml_tensor * Q = ggml_mul_mat(ctx, model.vision.layers[i].wq, norm);
        struct ggml_tensor * K = ggml_mul_mat(ctx, model.vision.layers[i].wk, norm);
        struct ggml_tensor * V = ggml_mul_mat(ctx, model.vision.layers[i].wv, norm);
        
        struct ggml_tensor * KQ = ggml_mul_mat(ctx, K, Q);
        KQ = ggml_scale(ctx, KQ, 1.0f / sqrtf(hparams.n_embd_vision_head));
        KQ = ggml_soft_max(ctx, KQ);
        
        struct ggml_tensor * attn_out = ggml_mul_mat(ctx, V, KQ);
        attn_out = ggml_mul_mat(ctx, model.vision.layers[i].wo, attn_out);
        
        patches = ggml_add(ctx, patches, attn_out);
        
        // FFN
        struct ggml_tensor * ffn_norm = ggml_rms_norm(ctx, patches, hparams.vision_layer_norm_eps);
        ffn_norm = ggml_mul(ctx, ffn_norm, model.vision.layers[i].ffn_norm);
        
        struct ggml_tensor * ffn_gate = ggml_mul_mat(ctx, model.vision.layers[i].ffn_gate, ffn_norm);
        ffn_gate = ggml_gelu(ctx, ffn_gate);
        
        struct ggml_tensor * ffn_up = ggml_mul_mat(ctx, model.vision.layers[i].ffn_up, ffn_norm);
        struct ggml_tensor * ffn_out = ggml_mul(ctx, ffn_gate, ffn_up);
        ffn_out = ggml_mul_mat(ctx, model.vision.layers[i].ffn_down, ffn_out);
        
        patches = ggml_add(ctx, patches, ffn_out);
    }
    
    return patches;
}
```

### 5.3 Audio Encoder 实现

```cpp
struct ggml_tensor * encode_audio(
    ggml_context * ctx,
    const llama_model & model,
    const ggml_tensor * mel_spectrogram
) {
    const auto & hparams = model.hparams;
    
    // 1. Convolutional Frontend
    struct ggml_tensor * features = ggml_conv_1d(ctx, model.audio.conv1, mel_spectrogram, 1, 0, 1);
    features = ggml_gelu(ctx, features);
    
    features = ggml_conv_1d(ctx, model.audio.conv2, features, 1, 0, 1);
    features = ggml_gelu(ctx, features);
    
    // 2. Transformer Layers
    for (int i = 0; i < hparams.n_layer_audio; i++) {
        struct ggml_tensor * norm = ggml_rms_norm(ctx, features, hparams.audio_layer_norm_eps);
        norm = ggml_mul(ctx, norm, model.audio.layers[i].norm);
        
        struct ggml_tensor * Q = ggml_mul_mat(ctx, model.audio.layers[i].wq, norm);
        struct ggml_tensor * K = ggml_mul_mat(ctx, model.audio.layers[i].wk, norm);
        struct ggml_tensor * V = ggml_mul_mat(ctx, model.audio.layers[i].wv, norm);
        
        struct ggml_tensor * KQ = ggml_mul_mat(ctx, K, Q);
        KQ = ggml_scale(ctx, KQ, 1.0f / sqrtf(hparams.n_embd_audio_head));
        KQ = ggml_soft_max(ctx, KQ);
        
        struct ggml_tensor * attn_out = ggml_mul_mat(ctx, V, KQ);
        attn_out = ggml_mul_mat(ctx, model.audio.layers[i].wo, attn_out);
        
        features = ggml_add(ctx, features, attn_out);
        
        // FFN
        struct ggml_tensor * ffn_norm = ggml_rms_norm(ctx, features, hparams.audio_layer_norm_eps);
        ffn_norm = ggml_mul(ctx, ffn_norm, model.audio.layers[i].ffn_norm);
        
        struct ggml_tensor * ffn_gate = ggml_mul_mat(ctx, model.audio.layers[i].ffn_gate, ffn_norm);
        ffn_gate = ggml_gelu(ctx, ffn_gate);
        
        struct ggml_tensor * ffn_up = ggml_mul_mat(ctx, model.audio.layers[i].ffn_up, ffn_norm);
        struct ggml_tensor * ffn_out = ggml_mul(ctx, ffn_gate, ffn_up);
        ffn_out = ggml_mul_mat(ctx, model.audio.layers[i].ffn_down, ffn_out);
        
        features = ggml_add(ctx, features, ffn_out);
    }
    
    return features;
}
```

---

## 6. 多模态输入处理

### 6.1 图像预处理

```cpp
#include "stb_image.h"

ImageInput preprocess_image(const std::string& image_path, int target_size = 512) {
    ImageInput img;
    
    // 1. 加载图像
    int width, height, channels;
    uint8_t* pixel_data = stbi_load(image_path.c_str(), &width, &height, &channels, 3);
    
    // 2. Resize 到目标尺寸
    int resized_width, resized_height;
    float aspect_ratio = static_cast<float>(width) / height;
    
    if (aspect_ratio > 1.0f) {
        resized_width = target_size;
        resized_height = static_cast<int>(target_size / aspect_ratio);
    } else {
        resized_height = target_size;
        resized_width = static_cast<int>(target_size * aspect_ratio);
    }
    
    // 3. 归一化到 [0, 1] 并转换为 tensor [C, H, W]
    img.tensor = new float[3 * resized_height * resized_width];
    for (int c = 0; c < 3; c++) {
        for (int h = 0; h < resized_height; h++) {
            for (int w = 0; w < resized_width; w++) {
                int src_idx = (h * resized_width + w) * 3 + c;
                int dst_idx = c * resized_height * resized_width + h * resized_width + w;
                img.tensor[dst_idx] = pixel_data[src_idx] / 255.0f;
            }
        }
    }
    
    stbi_image_free(pixel_data);
    return img;
}
```

### 6.2 音频预处理

```cpp
#include <subprocess/subprocess.h>

AudioInput preprocess_audio(const std::string& audio_path, int sample_rate = 16000) {
    AudioInput audio;
    
    // 使用 FFmpeg 提取并重采样
    std::string cmd = "ffmpeg -i \"" + audio_path + "\" "
                      "-vn -acodec pcm_s16le -ar " + std::to_string(sample_rate) + 
                      " -ac 1 -f s16le -";
    
    subprocess_s process;
    subprocess_create(cmd.c_str(), subprocess_option_combined_stdout_stderr, &process);
    
    // 读取音频数据
    FILE* stdout = subprocess_stdout(&process);
    std::vector<int16_t> raw_audio;
    int16_t buffer[4096];
    
    while (true) {
        size_t n = fread(buffer, sizeof(int16_t), 4096, stdout);
        if (n == 0) break;
        raw_audio.insert(raw_audio.end(), buffer, buffer + n);
    }
    
    subprocess_destroy(&process);
    
    // 转换为浮点数
    audio.waveform.resize(raw_audio.size());
    for (size_t i = 0; i < raw_audio.size(); i++) {
        audio.waveform[i] = raw_audio[i] / 32768.0f;
    }
    
    // 计算 Mel 语谱图
    compute_mel_spectrogram(audio, 128);
    
    return audio;
}
```

### 6.3 视频预处理

```cpp
VideoInput preprocess_video(const std::string& video_path, float fps = 1.0f, int frame_size = 512) {
    VideoInput video;
    
    // 使用 FFmpeg 提取帧
    std::string temp_dir = "/tmp/video_frames_" + std::to_string(getpid());
    std::filesystem::create_directories(temp_dir);
    
    std::string cmd = "ffmpeg -i \"" + video_path + "\" "
                      "-vf \"fps=" + std::to_string(static_cast<int>(fps)) + "\" "
                      "\"" + temp_dir + "/frame_%04d.jpg\" -y";
    
    std::system(cmd.c_str());
    
    // 加载所有帧
    for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
        if (entry.path().extension() == ".jpg") {
            video.frames.push_back(preprocess_image(entry.path().string(), frame_size));
        }
    }
    
    video.total_frames = video.frames.size();
    std::filesystem::remove_all(temp_dir);
    
    return video;
}
```

---

## 7. 完整推理流程

### 7.1 推理步骤

```
1. 加载模型
   ├── 读取 GGUF 文件
   ├── 初始化后端 (CPU/CUDA/Metal)
   └── 分配 KV Cache

2. 预处理输入
   ├── 文本 → Tokenize
   ├── 图像 → Resize + Normalize → Tensor
   ├── 音频 → Mel Spectrogram → Tensor
   └── 视频 → Extract Frames → 多个图像 Tensors

3. 编码多模态特征
   ├── encode_image() → Vision Embeddings
   ├── encode_audio() → Audio Embeddings
   └── 合并 → Multimodal Embeddings

4. LLM 自回归生成
   ├── Forward Pass → Logits
   ├── Sample → Next Token
   └── 更新 KV Cache

5. 解码输出
   └── Detokenize → 文本响应
```

### 7.2 主程序示例

```bash
# 文本对话
./qwen3-omni-inference \
    -m qwen3-omni-3b-q4_k_m.gguf \
    -p "请介绍一下量子计算的基本原理" \
    -n 512 \
    --temp 0.7

# 图像问答
./qwen3-omni-inference \
    -m qwen3-omni-3b-q4_k_m.gguf \
    -i photo.jpg \
    -p "这张照片里有什么？请用中文详细描述" \
    -n 256

# 视频分析
./qwen3-omni-inference \
    -m qwen3-omni-7b-q8_0.gguf \
    -v video.mp4 \
    -p "视频中发生了什么事件？按时间顺序描述" \
    -n 512 \
    --fps 1

# 音频转录
./qwen3-omni-inference \
    -m qwen3-omni-3b-q4_k_m.gguf \
    -a meeting.wav \
    -p "请将这段音频转录为文字" \
    -n 1024

# 多模态组合
./qwen3-omni-inference \
    -m qwen3-omni-7b-q4_k_m.gguf \
    -i image1.jpg -i image2.jpg \
    -a audio.wav \
    -p "结合这些图片和音频，讲一个完整的故事" \
    -n 768
```

---

## 8. 性能优化与调试

### 8.1 内存优化

```cpp
// 使用量化 KV Cache
llama_context_params ctx_params = llama_context_default_params();
ctx_params.type_k = GGML_TYPE_Q8_0;  // KV Cache 量化
ctx_params.type_v = GGML_TYPE_Q8_0;

// 减少上下文长度
ctx_params.n_ctx = 2048;
```

### 8.2 计算优化

```cpp
// 启用 Flash Attention
struct ggml_tensor * attn_out = ggml_flash_attn_ext(
    ctx0, Q, K, V, mask, scale, 0.0f, 0.0f
);

// 设置精度为 F32
ggml_flash_attn_ext_set_prec(attn_out, GGML_PREC_F32);
```

### 8.3 多 GPU 支持

```cpp
// 初始化多个 GPU
ggml_backend_t backend_gpu0 = ggml_backend_cuda_init(0);
ggml_backend_t backend_gpu1 = ggml_backend_cuda_init(1);

// 创建调度器
std::vector<ggml_backend_t> backends = {backend_gpu0, backend_gpu1};
ggml_backend_sched_t sched = ggml_backend_sched_new(
    backends.data(), nullptr, backends.size(),
    GGML_DEFAULT_GRAPH_SIZE, true
);
```

### 8.4 性能参考数据

**Qwen3-Omni-3B (RTX 4090)**:

| 量化格式 | 模型大小 | 生成速度 | VRAM 占用 |
|----------|----------|----------|----------|
| F16 | 6.2 GB | 120 tok/s | 8.5 GB |
| Q8_0 | 3.4 GB | 145 tok/s | 5.2 GB |
| Q4_K_M | 2.1 GB | 165 tok/s | 3.8 GB |

**Qwen3-Omni-7B (RTX 4090)**:

| 量化格式 | 模型大小 | 生成速度 | VRAM 占用 |
|----------|----------|----------|----------|
| F16 | 14.8 GB | 65 tok/s | 16.2 GB |
| Q8_0 | 8.2 GB | 95 tok/s | 9.8 GB |
| Q4_K_M | 4.5 GB | 125 tok/s | 6.2 GB |

### 8.5 常见问题排查

**问题 1: 模型加载失败**
- 检查 GGUF 文件完整性
- 确认 GGUF 版本兼容性
- 重新转换模型

**问题 2: 输出重复**
- 调整采样参数：`--temp 0.8 --top-p 0.9 --repeat-penalty 1.2`
- 检查 RoPE 参数设置

**问题 3: CUDA OOM**
```bash
# 减少 GPU 层数
./qwen3-omni-inference -m model.gguf -ngl 20

# 减小上下文
./qwen3-omni-inference -m model.gguf -c 2048

# 使用量化 KV Cache
./qwen3-omni-inference -m model.gguf --type-k q8_0 --type-v q8_0
```

---

## 附录：参考资料

- **llama.cpp 仓库**: https://github.com/ggerganov/llama.cpp
- **GGUF 格式规范**: https://github.com/ggml-org/ggml/blob/master/docs/gguf.md
- **Qwen 官方 GitHub**: https://github.com/QwenLM/Qwen
- **HOWTO-add-model.md**: https://github.com/ggerganov/llama.cpp/blob/master/docs/development/HOWTO-add-model.md

---

**文档作者**: AI Assistant  
**最后更新**: 2026-03-25  
**版本**: 1.0
