# Stable Diffusion 模型 GGML 推理开发指南

## 目录

1. [概述](#1-概述)
2. [SD2.x 模型架构详解](#2-sd2x-模型架构详解)
3. [GGML 算子支持分析](#3-ggml-算子支持分析)
4. [开发准备工作](#4-开发准备工作)
5. [模型格式转换](#5-模型格式转换)
6. [核心组件实现](#6-核心组件实现)
7. [完整推理流程](#7-完整推理流程)
8. [性能优化策略](#8-性能优化策略)
9. [测试与验证](#9-测试与验证)

---

## 1. 概述

### 1.1 目标

在 GGML/llama.cpp 框架上实现 Stable Diffusion 2.x 模型的完整推理能力，包括：
- Text Encoder (CLIP ViT-L/14)
- UNet (Latent Diffusion Model)
- VAE Decoder

### 1.2 Stable Diffusion 工作流程

```
文本输入 → CLIP Text Encoder → Text Embeddings (77x1024)
                                       ↓
随机噪声 → UNet (扩散去噪, 20-50步) → 去噪后 Latent
                                       ↓
                              VAE Decoder → 生成图像 (512x512 或 768x768)
```

### 1.3 SD2.x 与 SD1.x 的主要区别

| 特性 | SD 1.x | SD 2.x |
|------|--------|--------|
| Text Encoder | CLIP ViT-L/14 (OpenAI) | OpenCLIP ViT-H/14 或 ViT-bigG/14 |
| Text Embedding 维度 | 768 | 1024 (ViT-H) 或 1280 (ViT-bigG) |
| 默认分辨率 | 512x512 | 512x512 或 768x768 |
| UNet 结构 | 类似，但参数不同 | v-prediction 目标 |
| VAE | KL-f8 | 相同结构，不同权重 |

---

## 2. SD2.x 模型架构详解

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                    Stable Diffusion Pipeline                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐    │
│  │ CLIP Text    │     │    UNet      │     │  VAE Decoder │    │
│  │ Encoder      │────→│ (Denoiser)   │────→│              │    │
│  │              │     │              │     │              │    │
│  │ Transformer  │     │ Transformer  │     │   Decoder    │    │
│  │ Layers x32   │     │ + Conv       │     │   + Conv     │    │
│  └──────────────┘     └──────────────┘     └──────────────┘    │
│        ↑                    ↑                    ↑              │
│   Text Prompt         Noise Latent        Latent Space         │
│                       + Timestep                               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 CLIP Text Encoder 架构

```cpp
// CLIP ViT-H/14 结构
struct CLIPTextEncoder {
    // Token Embedding: vocab_size × hidden_dim
    struct ggml_tensor * token_embedding;  // [49408, 1024]
    
    // Position Embedding: max_seq_len × hidden_dim
    struct ggml_tensor * position_embedding;  // [77, 1024]
    
    // Transformer Layers (32 layers for ViT-H)
    struct CLIPLayer {
        // Self-Attention
        struct ggml_tensor * attention_qkv_weight;  // [1024, 3*1024] 或分离
        struct ggml_tensor * attention_qkv_bias;
        struct ggml_tensor * attention_out_weight;  // [1024, 1024]
        struct ggml_tensor * attention_out_bias;
        
        // Layer Norm 1 & 2
        struct ggml_tensor * ln1_weight;
        struct ggml_tensor * ln1_bias;
        struct ggml_tensor * ln2_weight;
        struct ggml_tensor * ln2_bias;
        
        // FFN (MLP)
        struct ggml_tensor * ffn_up_weight;   // [1024, 4096]
        struct ggml_tensor * ffn_up_bias;
        struct ggml_tensor * ffn_down_weight; // [4096, 1024]
        struct ggml_tensor * ffn_down_bias;
    } layers[32];
    
    // Final Layer Norm
    struct ggml_tensor * final_ln_weight;
    struct ggml_tensor * final_ln_bias;
    
    // Text Projection (用于 pooled output)
    struct ggml_tensor * text_projection;  // [1024, 1024]
};
```

### 2.3 UNet 架构

```cpp
// SD2.x UNet 结构
struct UNetModel {
    // ============ 输入投影 ============
    // Conv In: 将 latent 从 4ch 转换到 320ch
    struct ggml_tensor * conv_in_weight;  // [320, 4, 3, 3]
    struct ggml_tensor * conv_in_bias;    // [320]
    
    // Time Embedding
    struct ggml_tensor * time_embed_linear1_weight;  // [320, 1280]
    struct ggml_tensor * time_embed_linear1_bias;    // [1280]
    struct ggml_tensor * time_embed_linear2_weight;  // [1280, 1280]
    struct ggml_tensor * time_embed_linear2_bias;    // [1280]
    
    // Text Conditioning Projection
    struct ggml_tensor * proj_in_weight;  // [1024, 1024] 或 cross_attn 维度
    
    // ============ 下采样路径 (Encoder) ============
    // DownBlock 1: 320ch, 2 个 ResBlock + 2 个 CrossAttn
    // DownBlock 2: 640ch, 2 个 ResBlock + 2 个 CrossAttn  
    // DownBlock 3: 1280ch, 2 个 ResBlock + 2 个 CrossAttn
    // DownBlock 4: 1280ch, 2 个 ResBlock (无 attention)
    
    // ============ 中间块 (Middle Block) ============
    // MiddleBlock: 1280ch, 1 ResBlock + 1 CrossAttn + 1 ResBlock
    
    // ============ 上采样路径 (Decoder) ============
    // UpBlock 1: 1280ch, 3 个 ResBlock + 3 个 CrossAttn
    // UpBlock 2: 1280ch, 3 个 ResBlock + 3 个 CrossAttn
    // UpBlock 3: 640ch, 3 个 ResBlock + 3 个 CrossAttn
    // UpBlock 4: 320ch, 3 个 ResBlock (无 attention)
    
    // ============ 输出投影 ============
    struct ggml_tensor * conv_norm_out_weight;  // [320]
    struct ggml_tensor * conv_norm_out_bias;    // [320]
    struct ggml_tensor * conv_out_weight;       // [4, 320, 3, 3]
    struct ggml_tensor * conv_out_bias;         // [4]
};

// ResBlock 结构
struct ResBlock {
    // Group Norm
    struct ggml_tensor * norm1_weight;  // [in_channels]
    struct ggml_tensor * norm1_bias;
    
    // Conv 3x3
    struct ggml_tensor * conv1_weight;  // [out_channels, in_channels, 3, 3]
    struct ggml_tensor * conv1_bias;
    
    // Time Embedding Projection
    struct ggml_tensor * time_emb_proj_weight;  // [out_channels, temb_channels]
    struct ggml_tensor * time_emb_proj_bias;
    
    // Group Norm 2
    struct ggml_tensor * norm2_weight;
    struct ggml_tensor * norm2_bias;
    
    // Conv 3x3
    struct ggml_tensor * conv2_weight;  // [out_channels, out_channels, 3, 3]
    struct ggml_tensor * conv2_bias;
    
    // Skip Connection Conv (如果 in_channels != out_channels)
    struct ggml_tensor * conv_shortcut_weight;
    struct ggml_tensor * conv_shortcut_bias;
};

// CrossAttention 结构
struct CrossAttention {
    // Group Norm
    struct ggml_tensor * norm_weight;
    struct ggml_tensor * norm_bias;
    
    // Q, K, V Projections
    struct ggml_tensor * to_q_weight;  // [inner_dim, query_dim]
    struct ggml_tensor * to_k_weight;  // [inner_dim, context_dim]
    struct ggml_tensor * to_v_weight;  // [inner_dim, context_dim]
    
    // Output Projection
    struct ggml_tensor * to_out_weight;  // [query_dim, inner_dim]
    struct ggml_tensor * to_out_bias;
};
```

### 2.4 VAE Decoder 架构

```cpp
struct VAEDecoder {
    // Conv In
    struct ggml_tensor * conv_in_weight;  // [512, 4, 3, 3]
    struct ggml_tensor * conv_in_bias;    // [512]
    
    // ResBlocks + Upsample
    // Block 1: 512ch, 3 ResBlocks
    // Block 2: 512ch → 256ch, 3 ResBlocks + Upsample
    // Block 3: 256ch → 128ch, 3 ResBlocks + Upsample
    // Block 4: 128ch → 128ch, 3 ResBlocks + Upsample
    
    // Conv Out
    struct ggml_tensor * conv_norm_out_weight;  // [128]
    struct ggml_tensor * conv_norm_out_bias;
    struct ggml_tensor * conv_out_weight;       // [3, 128, 3, 3]
    struct ggml_tensor * conv_out_bias;         // [3]
};

// VAE ResBlock
struct VAEResBlock {
    struct ggml_tensor * norm1_weight;
    struct ggml_tensor * norm1_bias;
    struct ggml_tensor * conv1_weight;
    struct ggml_tensor * conv1_bias;
    
    struct ggml_tensor * norm2_weight;
    struct ggml_tensor * norm2_bias;
    struct ggml_tensor * conv2_weight;
    struct ggml_tensor * conv2_bias;
    
    // 如果通道数变化
    struct ggml_tensor * nin_shortcut_weight;  // 1x1 conv
    struct ggml_tensor * nin_shortcut_bias;
};
```

---

## 3. GGML 算子支持分析

### 3.1 已支持算子清单

| 算子类型 | GGML 算子 | SD 需求 | 支持状态 |
|----------|-----------|---------|----------|
| **卷积类** | `GGML_OP_CONV_2D` | Conv2d | ✅ 已支持 |
| | `GGML_OP_CONV_TRANSPOSE_2D` | Upsample Conv | ✅ 已支持 |
| | `GGML_OP_CONV_2D_DW` | Depthwise Conv | ✅ 已支持 |
| | `GGML_OP_IM2COL` | Conv im2col | ✅ 已支持 |
| **归一化** | `GGML_OP_GROUP_NORM` | GroupNorm | ✅ 已支持 |
| | `GGML_OP_NORM` | LayerNorm | ✅ 已支持 |
| **注意力** | `GGML_OP_SOFT_MAX` | Attention | ✅ 已支持 |
| | `GGML_OP_FLASH_ATTN_EXT` | Flash Attn | ✅ 已支持 (优化) |
| **激活函数** | `GGML_UNARY_OP_SILU` | SiLU/Swish | ✅ 已支持 |
| | `GGML_UNARY_OP_GELU` | GeGLU | ✅ 已支持 |
| | `GGML_UNARY_OP_SIGMOID` | Gating | ✅ 已支持 |
| **形状操作** | `GGML_OP_RESHAPE` | Reshape | ✅ 已支持 |
| | `GGML_OP_PERMUTE` | Permute | ✅ 已支持 |
| | `GGML_OP_TRANSPOSE` | Transpose | ✅ 已支持 |
| | `GGML_OP_VIEW` | View/Slice | ✅ 已支持 |
| **上下采样** | `GGML_OP_UPSCALE` | Upsample | ✅ 已支持 |
| | `GGML_OP_PAD` | Padding | ✅ 已支持 |
| | `GGML_OP_POOL_2D` | AvgPool | ✅ 已支持 |
| **时间步** | `GGML_OP_TIMESTEP_EMBEDDING` | Timestep | ✅ 已支持 |
| **基础运算** | `GGML_OP_ADD` | Add | ✅ 已支持 |
| | `GGML_OP_MUL` | Multiply | ✅ 已支持 |
| | `GGML_OP_SCALE` | Scale | ✅ 已支持 |
| | `GGML_OP_MUL_MAT` | Linear | ✅ 已支持 |

### 3.2 需要验证/扩展的算子

| 算子 | 说明 | 优先级 |
|------|------|--------|
| Cross Attention | 需要组合实现 Q@K^T@V | 高 |
| Timestep Sinusoidal Embedding | 已有 `ggml_timestep_embedding` | 高 |
| Group Normalization | 已支持，需验证正确性 | 高 |
| Pixel Shuffle / Rearrange | VAE 上采样可能需要 | 中 |
| Sliced Attention | 大分辨率内存优化 | 中 |

### 3.3 算子实现验证代码

```cpp
// 测试 GROUP_NORM 正确性
void test_group_norm() {
    struct ggml_init_params params = {
        .mem_size = 16 * 1024 * 1024,
        .mem_buffer = NULL,
    };
    struct ggml_context * ctx = ggml_init(params);
    
    // 创建输入: [C, H, W, N] = [320, 64, 64, 1]
    struct ggml_tensor * x = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, 64, 64, 320, 1);
    
    // 创建 GroupNorm 参数
    struct ggml_tensor * weight = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 320);
    struct ggml_tensor * bias = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 320);
    
    // GroupNorm: num_groups = 32
    struct ggml_tensor * normalized = ggml_group_norm(ctx, x, weight, bias, 32, 1e-5f);
    
    // 验证计算结果...
}

// 测试 CONV_2D
void test_conv2d() {
    // 输入: [in_ch, H, W, batch] = [4, 64, 64, 1]
    struct ggml_tensor * input = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, 64, 64, 4, 1);
    
    // 权重: [out_ch, in_ch, kH, kW] = [320, 4, 3, 3]
    struct ggml_tensor * weight = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, 3, 3, 4, 320);
    
    // Conv2d: stride=1, padding=1
    struct ggml_tensor * conv_out = ggml_conv_2d(ctx, weight, input, 1, 1, 1, 1, 1, 1);
}

// 测试 TIMESTEP_EMBEDDING
void test_timestep_embedding() {
    // 时间步张量 [n_timesteps]
    struct ggml_tensor * timesteps = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 1);
    
    // 生成时间步嵌入: [dim] = [1280]
    struct ggml_tensor * emb = ggml_timestep_embedding(ctx, timesteps, 1280, 10000);
}
```

---

## 4. 开发准备工作

### 4.1 环境准备

```bash
# 克隆 llama.cpp
git clone https://github.com/ggerganov/llama.cpp.git
cd llama.cpp

# 编译 (启用 CUDA 加速)
mkdir build && cd build
cmake .. -DGGML_CUDA=ON -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 验证 GGML 编译
./bin/llama-cli --help
```

### 4.2 模型权重准备

```bash
# 下载 SD2.1 模型 (选择一个)
# Stable Diffusion v2.1-base
wget https://huggingface.co/stabilityai/stable-diffusion-2-1-base/resolve/main/v2-1_512-ema-pruned.ckpt

# Stable Diffusion v2.1-768
wget https://huggingface.co/stabilityai/stable-diffusion-2-1/resolve/main/v2-1_768-ema-pruned.ckpt
```

### 4.3 依赖工具

```bash
# Python 环境 (用于模型转换)
conda create -n sd_convert python=3.10
conda activate sd_convert

pip install torch torchvision safetensors transformers diffusers omegaconf
pip install gguf  # 用于 GGUF 格式写入
```

### 4.4 项目目录结构

```
sd-inference/
├── CMakeLists.txt
├── src/
│   ├── sd-common.h           # 通用定义
│   ├── sd-common.cpp
│   ├── sd-clip.h             # CLIP Text Encoder
│   ├── sd-clip.cpp
│   ├── sd-unet.h             # UNet Denoiser
│   ├── sd-unet.cpp
│   ├── sd-vae.h              # VAE Decoder
│   ├── sd-vae.cpp
│   ├── sd-pipeline.h         # 完整 Pipeline
│   ├── sd-pipeline.cpp
│   └── sd-main.cpp           # 主程序入口
├── convert/
│   ├── convert_sd_to_gguf.py # 模型转换脚本
│   └── verify_weights.py     # 权重验证
├── tests/
│   ├── test_ops.cpp          # 算子测试
│   ├── test_clip.cpp         # CLIP 测试
│   ├── test_unet.cpp         # UNet 测试
│   └── test_vae.cpp          # VAE 测试
└── models/
    └── (GGUF 模型文件)
```

---

## 5. 模型格式转换

### 5.1 GGUF 格式设计

```python
# GGUF 键名设计
GGUF_KEYS = {
    # ========== 元信息 ==========
    "sd.version": "2.1",
    "sd.resolution": 768,
    "sd.v_prediction": True,  # SD2.x 使用 v-prediction
    
    # ========== CLIP Text Encoder ==========
    "clip.hidden_size": 1024,
    "clip.intermediate_size": 4096,
    "clip.num_attention_heads": 16,
    "clip.num_hidden_layers": 32,
    "clip.vocab_size": 49408,
    "clip.max_position_embeddings": 77,
    
    # ========== UNet ==========
    "unet.in_channels": 4,
    "unet.out_channels": 4,
    "unet.sample_size": 96,  # 96 * 8 = 768
    "unet.cross_attention_dim": 1024,
    "unet.attention_head_dim": 64,
    "unet.num_attention_heads": [5, 10, 20, 20],  # 各层头数
    
    # ========== VAE ==========
    "vae.z_channels": 4,
    "vae.in_channels": 3,
    "vae.out_channels": 3,
    "vae.channel_multiplier": [1, 2, 4, 4],
    "vae.num_res_blocks": 2,
}

# 张量命名规范
TENSOR_NAMES = {
    # CLIP
    "clip.token_embedding.weight": "model.token_embedding.weight",
    "clip.position_embedding.weight": "model.position_embedding.weight",
    "clip.layers.{i}.self_attn.qkv.weight": "model.layers.{i}.self_attn.qkv.weight",
    "clip.layers.{i}.mlp.fc1.weight": "model.layers.{i}.mlp.fc1.weight",
    
    # UNet
    "unet.conv_in.weight": "unet.conv_in.weight",
    "unet.time_embed.0.weight": "unet.time_embed.0.weight",
    "unet.down_blocks.{i}.resnets.{j}.conv1.weight": "unet.down.{i}.res.{j}.conv1.weight",
    "unet.down_blocks.{i}.attentions.{j}.to_q.weight": "unet.down.{i}.attn.{j}.to_q.weight",
    
    # VAE
    "vae.decoder.conv_in.weight": "vae.decoder.conv_in.weight",
    "vae.decoder.up_blocks.{i}.resnets.{j}.conv1.weight": "vae.up.{i}.res.{j}.conv1.weight",
}
```

### 5.2 转换脚本核心代码

```python
# convert/convert_sd_to_gguf.py

import torch
import numpy as np
from gguf import GGUFWriter, GGMLQuantizationType

def convert_sd_to_gguf(checkpoint_path, output_path, quantize=False):
    """将 SD checkpoint 转换为 GGUF 格式"""
    
    # 加载原始权重
    if checkpoint_path.endswith('.safetensors'):
        from safetensors.torch import load_file
        state_dict = load_file(checkpoint_path)
    else:
        checkpoint = torch.load(checkpoint_path, map_location='cpu')
        state_dict = checkpoint['state_dict']
    
    # 分离各组件权重
    clip_weights = {}
    unet_weights = {}
    vae_weights = {}
    
    for name, tensor in state_dict.items():
        if name.startswith('cond_stage_model.'):
            clip_weights[name.replace('cond_stage_model.', '')] = tensor
        elif name.startswith('model.diffusion_model.'):
            unet_weights[name.replace('model.diffusion_model.', '')] = tensor
        elif name.startswith('first_stage_model.'):
            vae_weights[name.replace('first_stage_model.', '')] = tensor
    
    # 创建 GGUF Writer
    writer = GGUFWriter(output_path, "stable-diffusion-v2")
    
    # 添加元数据
    writer.add_string("sd.version", "2.1")
    writer.add_uint32("sd.resolution", 768)
    writer.add_bool("sd.v_prediction", True)
    
    # 写入 CLIP 权重
    write_clip_weights(writer, clip_weights, quantize)
    
    # 写入 UNet 权重
    write_unet_weights(writer, unet_weights, quantize)
    
    # 写入 VAE 权重
    write_vae_weights(writer, vae_weights, quantize)
    
    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()

def write_clip_weights(writer, weights, quantize):
    """写入 CLIP Text Encoder 权重"""
    
    # Token Embedding
    if 'model.token_embedding.weight' in weights:
        tensor = weights['model.token_embedding.weight']
        writer.add_tensor(
            "clip.token_embedding.weight",
            tensor.numpy(),
            raw_dtype=GGMLQuantizationType.F16 if quantize else GGMLQuantizationType.F32
        )
    
    # Position Embedding
    if 'model.positional_embedding' in weights:
        tensor = weights['model.positional_embedding']
        writer.add_tensor(
            "clip.position_embedding.weight",
            tensor.numpy()
        )
    
    # Transformer Layers
    for i in range(32):
        prefix = f'model.transformer.resblocks.{i}'
        
        # Attention QKV (可能合并或分离)
        if f'{prefix}.attn.in_proj_weight' in weights:
            qkv_weight = weights[f'{prefix}.attn.in_proj_weight']
            writer.add_tensor(
                f"clip.layers.{i}.self_attn.qkv.weight",
                qkv_weight.numpy()
            )
        
        # Attention Out
        if f'{prefix}.attn.out_proj.weight' in weights:
            writer.add_tensor(
                f"clip.layers.{i}.self_attn.out_proj.weight",
                weights[f'{prefix}.attn.out_proj.weight'].numpy()
            )
        
        # Layer Norm
        if f'{prefix}.ln_1.weight' in weights:
            writer.add_tensor(
                f"clip.layers.{i}.layer_norm1.weight",
                weights[f'{prefix}.ln_1.weight'].numpy()
            )
        
        # FFN
        if f'{prefix}.mlp.c_fc.weight' in weights:
            writer.add_tensor(
                f"clip.layers.{i}.mlp.fc1.weight",
                weights[f'{prefix}.mlp.c_fc.weight'].numpy()
            )
```

### 5.3 权重量化策略

```python
def quantize_weights(tensor, quant_type='Q8_0'):
    """对权重进行量化"""
    
    if quant_type == 'Q8_0':
        # Q8_0: 8-bit 量化，每 32 元素一个缩放因子
        from ggml import quantize_q8_0
        return quantize_q8_0(tensor)
    
    elif quant_type == 'Q4_K':
        # Q4_K: K-quant 4-bit
        from ggml import quantize_q4_k
        return quantize_q4_k(tensor)
    
    elif quant_type == 'F16':
        # FP16 (推荐用于激活敏感层)
        return tensor.half()

# 推荐的量化策略
QUANTIZE_STRATEGY = {
    # CLIP: 可以使用较高压缩
    'clip': {
        'token_embedding': 'Q4_K',  # 嵌入层
        'attention': 'Q8_0',        # 注意力权重
        'mlp': 'Q8_0',              # MLP 权重
    },
    
    # UNet: 需要保持精度
    'unet': {
        'conv': 'Q8_0',             # 卷积层
        'attention': 'Q8_0',        # 注意力
        'time_embed': 'F16',        # 时间嵌入
    },
    
    # VAE: 需要 F16 保持图像质量
    'vae': {
        'all': 'F16',               # 全部使用 FP16
    }
}
```

---

## 6. 核心组件实现

### 6.1 数据结构定义

```cpp
// src/sd-common.h

#pragma once

#include "ggml.h"
#include "ggml-backend.h"
#include <string>
#include <vector>
#include <memory>

namespace sd {

// 模型超参数
struct SDConfig {
    // CLIP
    int32_t clip_hidden_size = 1024;
    int32_t clip_intermediate_size = 4096;
    int32_t clip_num_layers = 32;
    int32_t clip_num_heads = 16;
    int32_t clip_vocab_size = 49408;
    int32_t clip_max_seq_len = 77;
    
    // UNet
    int32_t unet_in_channels = 4;
    int32_t unet_out_channels = 4;
    int32_t unet_sample_size = 96;  // latent size
    int32_t unet_cross_attn_dim = 1024;
    std::vector<int32_t> unet_attention_head_dim = {5, 10, 20, 20};
    std::vector<int32_t> unet_layers_per_block = {2, 2, 2, 2};
    
    // VAE
    int32_t vae_z_channels = 4;
    int32_t vae_channels = 128;
    std::vector<int32_t> vae_channel_mult = {1, 2, 4, 4};
    int32_t vae_num_res_blocks = 2;
    
    // 采样
    int32_t sample_steps = 20;
    float guidance_scale = 7.5f;
    bool v_prediction = true;  // SD2.x 使用 v-prediction
    
    int32_t seed = -1;
};

// 扩散调度器
struct Scheduler {
    std::vector<float> alphas_cumprod;
    std::vector<float> betas;
    std::vector<float> timesteps;
    
    // Euler Discrete Scheduler (SD2.x 默认)
    static Scheduler create_euler(int num_steps, float beta_start = 0.00085f, float beta_end = 0.012f);
    
    // 获取指定时间步的参数
    float get_alpha_cumprod(int timestep) const;
    float get_beta(int timestep) const;
};

// 模型权重
struct SDModel {
    ggml_backend_t backend = nullptr;
    ggml_backend_buffer_t buffer = nullptr;
    ggml_context * ctx = nullptr;
    
    // CLIP Text Encoder 权重
    struct clip_weights_t {
        struct ggml_tensor * token_embedding;
        struct ggml_tensor * position_embedding;
        struct ggml_tensor * final_ln_weight;
        struct ggml_tensor * final_ln_bias;
        
        struct layer_t {
            struct ggml_tensor * qkv_weight;
            struct ggml_tensor * qkv_bias;
            struct ggml_tensor * out_weight;
            struct ggml_tensor * out_bias;
            struct ggml_tensor * ln1_weight;
            struct ggml_tensor * ln1_bias;
            struct ggml_tensor * ln2_weight;
            struct ggml_tensor * ln2_bias;
            struct ggml_tensor * ffn_up_weight;
            struct ggml_tensor * ffn_up_bias;
            struct ggml_tensor * ffn_down_weight;
            struct ggml_tensor * ffn_down_bias;
        };
        std::vector<layer_t> layers;
    } clip;
    
    // UNet 权重
    struct unet_weights_t {
        struct ggml_tensor * conv_in_weight;
        struct ggml_tensor * conv_in_bias;
        
        // Time embedding
        struct ggml_tensor * time_embed_0_weight;
        struct ggml_tensor * time_embed_0_bias;
        struct ggml_tensor * time_embed_2_weight;
        struct ggml_tensor * time_embed_2_bias;
        
        // Down blocks, Mid block, Up blocks
        // ... (详细定义见实现文件)
        
        struct ggml_tensor * conv_out_weight;
        struct ggml_tensor * conv_out_bias;
        struct ggml_tensor * conv_norm_weight;
        struct ggml_tensor * conv_norm_bias;
    } unet;
    
    // VAE Decoder 权重
    struct vae_weights_t {
        struct ggml_tensor * conv_in_weight;
        struct ggml_tensor * conv_in_bias;
        
        struct ggml_tensor * conv_out_weight;
        struct ggml_tensor * conv_out_bias;
        struct ggml_tensor * conv_norm_weight;
        struct ggml_tensor * conv_norm_bias;
        
        // Decoder blocks
        // ...
    } vae;
    
    bool load_from_gguf(const std::string & path, const SDConfig & config);
    void free();
};

} // namespace sd
```

### 6.2 CLIP Text Encoder 实现

```cpp
// src/sd-clip.cpp

#include "sd-clip.h"
#include "sd-common.h"

namespace sd {

struct ggml_tensor * clip_text_encoder(
    struct ggml_context * ctx,
    const clip_weights_t & weights,
    struct ggml_tensor * input_ids,  // [1, 77] - tokenized text
    int num_layers
) {
    // 1. Token Embedding
    // input_ids: [batch, seq_len] -> [seq_len, batch]
    struct ggml_tensor * hidden = ggml_get_rows(ctx, weights.token_embedding, input_ids);
    
    // 2. Add Position Embedding
    struct ggml_tensor * pos_emb = weights.position_embedding;
    hidden = ggml_add(ctx, hidden, pos_emb);
    
    // 3. Transformer Layers
    for (int i = 0; i < num_layers; i++) {
        const auto & layer = weights.layers[i];
        
        // 3.1 Pre-LayerNorm
        struct ggml_tensor * normed = ggml_norm(ctx, hidden);
        normed = ggml_mul(ctx, normed, layer.ln1_weight);
        normed = ggml_add(ctx, normed, layer.ln1_bias);
        
        // 3.2 Self-Attention
        struct ggml_tensor * attn_out = clip_self_attention(
            ctx, layer, normed, 16  // num_heads
        );
        
        // 3.3 Residual
        hidden = ggml_add(ctx, hidden, attn_out);
        
        // 3.4 Pre-LayerNorm for FFN
        normed = ggml_norm(ctx, hidden);
        normed = ggml_mul(ctx, normed, layer.ln2_weight);
        normed = ggml_add(ctx, normed, layer.ln2_bias);
        
        // 3.5 FFN (QuickGELU activation)
        struct ggml_tensor * ffn_out = ggml_mul_mat(ctx, layer.ffn_up_weight, normed);
        ffn_out = ggml_add(ctx, ffn_out, layer.ffn_up_bias);
        ffn_out = ggml_gelu_quick(ctx, ffn_out);  // QuickGELU
        ffn_out = ggml_mul_mat(ctx, layer.ffn_down_weight, ffn_out);
        ffn_out = ggml_add(ctx, ffn_out, layer.ffn_down_bias);
        
        // 3.6 Residual
        hidden = ggml_add(ctx, hidden, ffn_out);
    }
    
    // 4. Final LayerNorm
    hidden = ggml_norm(ctx, hidden);
    hidden = ggml_mul(ctx, hidden, weights.final_ln_weight);
    hidden = ggml_add(ctx, hidden, weights.final_ln_bias);
    
    return hidden;  // [seq_len, batch, hidden_dim]
}

struct ggml_tensor * clip_self_attention(
    struct ggml_context * ctx,
    const clip_weights_t::layer_t & layer,
    struct ggml_tensor * hidden,
    int num_heads
) {
    int hidden_dim = hidden->ne[0];
    int head_dim = hidden_dim / num_heads;
    
    // QKV projection (合并权重)
    struct ggml_tensor * qkv = ggml_mul_mat(ctx, layer.qkv_weight, hidden);
    qkv = ggml_add(ctx, qkv, layer.qkv_bias);
    
    // 分离 Q, K, V
    struct ggml_tensor * q = ggml_view_3d(ctx, qkv, head_dim, num_heads, hidden->ne[2],
                                          ggml_element_size(qkv) * head_dim,
                                          ggml_element_size(qkv) * hidden_dim,
                                          0);
    struct ggml_tensor * k = ggml_view_3d(ctx, qkv, head_dim, num_heads, hidden->ne[2],
                                          ggml_element_size(qkv) * head_dim,
                                          ggml_element_size(qkv) * hidden_dim,
                                          ggml_element_size(qkv) * hidden_dim);
    struct ggml_tensor * v = ggml_view_3d(ctx, qkv, head_dim, num_heads, hidden->ne[2],
                                          ggml_element_size(qkv) * head_dim,
                                          ggml_element_size(qkv) * hidden_dim,
                                          ggml_element_size(qkv) * hidden_dim * 2);
    
    // Scaled Dot-Product Attention
    // Attention = softmax(Q @ K^T / sqrt(d_k)) @ V
    struct ggml_tensor * attn_weights = ggml_mul_mat(ctx, k, q);
    attn_weights = ggml_scale(ctx, attn_weights, 1.0f / sqrtf((float)head_dim));
    attn_weights = ggml_soft_max(ctx, attn_weights);
    
    struct ggml_tensor * attn_out = ggml_mul_mat(ctx, v, attn_weights);
    
    // Reshape back
    attn_out = ggml_reshape_2d(ctx, attn_out, hidden_dim, hidden->ne[2]);
    
    // Output projection
    attn_out = ggml_mul_mat(ctx, layer.out_weight, attn_out);
    attn_out = ggml_add(ctx, attn_out, layer.out_bias);
    
    return attn_out;
}

} // namespace sd
```

### 6.3 UNet 实现

```cpp
// src/sd-unet.cpp

#include "sd-unet.h"
#include "sd-common.h"

namespace sd {

struct ggml_tensor * unet_forward(
    struct ggml_context * ctx,
    const unet_weights_t & weights,
    struct ggml_tensor * latent,      // [4, H, W, batch]
    struct ggml_tensor * timestep_emb, // [temb_dim]
    struct ggml_tensor * text_emb,    // [seq_len, batch, cross_dim]
    const SDConfig & config
) {
    // 1. Conv In: 4ch -> 320ch
    struct ggml_tensor * h = ggml_conv_2d(ctx, weights.conv_in_weight, latent, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv_in_bias);
    
    // 2. Time Embedding
    struct ggml_tensor * temb = ggml_timestep_embedding(ctx, timestep_emb, 1280, 10000);
    temb = ggml_silu(ctx, temb);
    temb = ggml_mul_mat(ctx, weights.time_embed_0_weight, temb);
    temb = ggml_add(ctx, temb, weights.time_embed_0_bias);
    temb = ggml_silu(ctx, temb);
    temb = ggml_mul_mat(ctx, weights.time_embed_2_weight, temb);
    temb = ggml_add(ctx, temb, weights.time_embed_2_bias);
    
    // 3. Down blocks
    std::vector<struct ggml_tensor *> down_features;
    down_features.push_back(h);
    
    // DownBlock 0: 320ch
    h = down_block(ctx, weights.down_blocks[0], h, temb, text_emb, config, 0);
    down_features.push_back(h);
    
    // DownBlock 1: 640ch (下采样)
    h = ggml_conv_2d(ctx, weights.downsamplers[0], h, 2, 2, 0, 0, 1, 1);
    h = down_block(ctx, weights.down_blocks[1], h, temb, text_emb, config, 1);
    down_features.push_back(h);
    
    // ... 继续其他 down blocks
    
    // 4. Middle block
    h = mid_block(ctx, weights.mid_block, h, temb, text_emb, config);
    
    // 5. Up blocks (带 skip connections)
    for (int i = config.unet_layers_per_block.size() - 1; i >= 0; i--) {
        struct ggml_tensor * skip = down_features.back();
        down_features.pop_back();
        
        // Concat skip connection
        h = ggml_concat(ctx, h, skip);
        
        // Up block
        h = up_block(ctx, weights.up_blocks[i], h, temb, text_emb, config, i);
        
        // Upsample (if not last)
        if (i > 0) {
            h = ggml_upscale(ctx, h, 2);  // Nearest neighbor upscale
            // 或使用 ConvTranspose2d
            // h = ggml_conv_transpose_2d(ctx, weights.upsamplers[i], h, ...);
        }
    }
    
    // 6. Output
    h = ggml_group_norm(ctx, h, weights.conv_norm_weight, weights.conv_norm_bias, 32, 1e-5f);
    h = ggml_silu(ctx, h);
    h = ggml_conv_2d(ctx, weights.conv_out_weight, h, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv_out_bias);
    
    return h;  // [4, H, W, batch]
}

struct ggml_tensor * res_block(
    struct ggml_context * ctx,
    const resblock_weights_t & weights,
    struct ggml_tensor * hidden,
    struct ggml_tensor * temb
) {
    // Group Norm 1
    struct ggml_tensor * h = ggml_group_norm(ctx, hidden, weights.norm1_weight, weights.norm1_bias, 32, 1e-5f);
    
    // SiLU activation
    h = ggml_silu(ctx, h);
    
    // Conv 1
    h = ggml_conv_2d(ctx, weights.conv1_weight, h, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv1_bias);
    
    // Add time embedding
    if (temb) {
        struct ggml_tensor * temb_proj = ggml_mul_mat(ctx, weights.time_emb_proj_weight, temb);
        temb_proj = ggml_add(ctx, temb_proj, weights.time_emb_proj_bias);
        h = ggml_add(ctx, h, temb_proj);
    }
    
    // Group Norm 2
    h = ggml_group_norm(ctx, h, weights.norm2_weight, weights.norm2_bias, 32, 1e-5f);
    h = ggml_silu(ctx, h);
    
    // Conv 2
    h = ggml_conv_2d(ctx, weights.conv2_weight, h, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv2_bias);
    
    // Skip connection
    if (weights.conv_shortcut_weight) {
        hidden = ggml_conv_2d(ctx, weights.conv_shortcut_weight, hidden, 1, 1, 0, 0, 1, 1);
        hidden = ggml_add(ctx, hidden, weights.conv_shortcut_bias);
    }
    
    // Residual
    h = ggml_add(ctx, h, hidden);
    
    return h;
}

struct ggml_tensor * cross_attention(
    struct ggml_context * ctx,
    const cross_attn_weights_t & weights,
    struct ggml_tensor * hidden,
    struct ggml_tensor * context,
    int num_heads
) {
    int hidden_dim = hidden->ne[0];
    int head_dim = hidden_dim / num_heads;
    
    // Group Norm
    struct ggml_tensor * h = ggml_group_norm(ctx, hidden, weights.norm_weight, weights.norm_bias, 32, 1e-5f);
    
    // Spatial reshape for attention: [C, H, W, B] -> [C, H*W*B]
    h = ggml_reshape_2d(ctx, h, hidden_dim, h->ne[1] * h->ne[2] * h->ne[3]);
    
    // Q projection (from hidden)
    struct ggml_tensor * q = ggml_mul_mat(ctx, weights.to_q_weight, h);
    
    // K, V projection (from context)
    struct ggml_tensor * k = ggml_mul_mat(ctx, weights.to_k_weight, context);
    struct ggml_tensor * v = ggml_mul_mat(ctx, weights.to_v_weight, context);
    
    // Reshape for multi-head attention
    // q: [head_dim, num_heads, seq_len]
    // k: [head_dim, num_heads, context_len]
    // v: [head_dim, num_heads, context_len]
    
    // Attention: softmax(Q @ K^T / scale) @ V
    struct ggml_tensor * scale = 1.0f / sqrtf((float)head_dim);
    
    // K^T @ Q -> [context_len, seq_len, num_heads]
    struct ggml_tensor * attn = ggml_mul_mat(ctx, k, q);
    attn = ggml_scale(ctx, attn, scale);
    attn = ggml_soft_max(ctx, attn);
    
    // V @ attn
    struct ggml_tensor * attn_out = ggml_mul_mat(ctx, v, attn);
    
    // Reshape back
    attn_out = ggml_reshape_2d(ctx, attn_out, hidden_dim, h->ne[1]);
    
    // Output projection
    attn_out = ggml_mul_mat(ctx, weights.to_out_weight, attn_out);
    attn_out = ggml_add(ctx, attn_out, weights.to_out_bias);
    
    // Reshape to spatial: [C, H*W*B] -> [C, H, W, B]
    attn_out = ggml_reshape_4d(ctx, attn_out, hidden->ne[1], hidden->ne[2], hidden_dim, hidden->ne[3]);
    
    // Residual
    return ggml_add(ctx, hidden, attn_out);
}

} // namespace sd
```

### 6.4 VAE Decoder 实现

```cpp
// src/sd-vae.cpp

#include "sd-vae.h"
#include "sd-common.h"

namespace sd {

struct ggml_tensor * vae_decoder(
    struct ggml_context * ctx,
    const vae_weights_t & weights,
    struct ggml_tensor * latent,  // [4, H, W, batch]
    const SDConfig & config
) {
    // 1. Conv In: 4ch -> 512ch (scaling factor)
    // latent 通常需要先 scaling: latent * 0.18215
    struct ggml_tensor * h = ggml_scale(ctx, latent, 0.18215f);
    h = ggml_conv_2d(ctx, weights.conv_in_weight, h, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv_in_bias);
    
    // 2. Decoder blocks with upsampling
    int channels = 512;
    int channel_mult_idx = 3;  // 从最高倍率开始
    
    for (int i = 0; i < 4; i++) {
        int num_blocks = config.vae_num_res_blocks;
        
        // ResBlocks
        for (int j = 0; j < num_blocks; j++) {
            h = vae_res_block(ctx, weights.decoder_blocks[i][j], h, channels);
        }
        
        // Upsample (除了最后一个 block)
        if (i < 3) {
            h = ggml_upscale(ctx, h, 2);
            // 或 ConvTranspose2d
            h = ggml_conv_transpose_2d_p0(ctx, weights.upsample_convs[i], h, 2);
            channels /= 2;
        }
    }
    
    // 3. Output: Group Norm + SiLU + Conv
    h = ggml_group_norm(ctx, h, weights.conv_norm_weight, weights.conv_norm_bias, 32, 1e-5f);
    h = ggml_silu(ctx, h);
    h = ggml_conv_2d(ctx, weights.conv_out_weight, h, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv_out_bias);
    
    // 4. 输出范围映射到 [0, 1] 或 [-1, 1]
    // h = ggml_sigmoid(ctx, h);  // [0, 1]
    // 或
    h = ggml_scale(ctx, h, 0.5f);
    h = ggml_add1(ctx, h, 0.5f);  // [-1, 1] -> [0, 1]
    
    return h;  // [3, H*8, W*8, batch] = [3, 768, 768, batch]
}

struct ggml_tensor * vae_res_block(
    struct ggml_context * ctx,
    const vae_resblock_weights_t & weights,
    struct ggml_tensor * hidden,
    int out_channels
) {
    // Group Norm 1
    struct ggml_tensor * h = ggml_group_norm(ctx, hidden, weights.norm1_weight, weights.norm1_bias, 32, 1e-5f);
    h = ggml_silu(ctx, h);
    
    // Conv 1
    h = ggml_conv_2d(ctx, weights.conv1_weight, h, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv1_bias);
    
    // Group Norm 2
    h = ggml_group_norm(ctx, h, weights.norm2_weight, weights.norm2_bias, 32, 1e-5f);
    h = ggml_silu(ctx, h);
    
    // Conv 2
    h = ggml_conv_2d(ctx, weights.conv2_weight, h, 1, 1, 1, 1, 1, 1);
    h = ggml_add(ctx, h, weights.conv2_bias);
    
    // Skip connection
    if (weights.skip_weight) {
        hidden = ggml_conv_2d(ctx, weights.skip_weight, hidden, 1, 1, 0, 0, 1, 1);
    }
    
    return ggml_add(ctx, h, hidden);
}

} // namespace sd
```

---

## 7. 完整推理流程

### 7.1 Pipeline 实现

```cpp
// src/sd-pipeline.cpp

#include "sd-pipeline.h"
#include "sd-common.h"

namespace sd {

class SDPipeline {
public:
    SDPipeline(const std::string & model_path, const SDConfig & config);
    ~SDPipeline();
    
    // 文生图
    ggml_tensor * text_to_image(
        const std::string & prompt,
        const std::string & negative_prompt = "",
        int seed = -1
    );
    
    // 图生图
    ggml_tensor * image_to_image(
        const std::string & prompt,
        ggml_tensor * init_image,
        float strength = 0.8f
    );
    
private:
    SDConfig config_;
    SDModel model_;
    Scheduler scheduler_;
    
    // 文本编码
    ggml_tensor * encode_text(const std::string & prompt);
    
    // 扩散采样
    ggml_tensor * diffusion_sample(
        ggml_tensor * text_emb,
        ggml_tensor * uncond_emb,
        int seed
    );
    
    // VAE 解码
    ggml_tensor * decode_latent(ggml_tensor * latent);
    
    // Tokenizer
    std::vector<int> tokenize(const std::string & text);
};

SDPipeline::SDPipeline(const std::string & model_path, const SDConfig & config)
    : config_(config) {
    
    // 初始化 backend
    model_.backend = ggml_backend_cuda_init(0);  // 使用 GPU
    if (!model_.backend) {
        model_.backend = ggml_backend_cpu_init();
    }
    
    // 加载模型
    model_.load_from_gguf(model_path, config_);
    
    // 创建 scheduler
    scheduler_ = Scheduler::create_euler(config_.sample_steps);
}

ggml_tensor * SDPipeline::text_to_image(
    const std::string & prompt,
    const std::string & negative_prompt,
    int seed
) {
    // 1. 文本编码
    ggml_tensor * text_emb = encode_text(prompt);
    ggml_tensor * uncond_emb = negative_prompt.empty() ? nullptr : encode_text(negative_prompt);
    
    // 2. 扩散采样
    ggml_tensor * latent = diffusion_sample(text_emb, uncond_emb, seed);
    
    // 3. VAE 解码
    ggml_tensor * image = decode_latent(latent);
    
    return image;
}

ggml_tensor * SDPipeline::diffusion_sample(
    ggml_tensor * text_emb,
    ggml_tensor * uncond_emb,
    int seed
) {
    // 初始化随机噪声
    if (seed < 0) seed = time(NULL);
    struct ggml_tensor * latent = init_random_noise(
        config_.unet_in_channels,
        config_.unet_sample_size,
        config_.unet_sample_size,
        1,  // batch
        seed
    );
    
    // 迭代去噪
    for (int i = 0; i < config_.sample_steps; i++) {
        int t = scheduler_.timesteps[i];
        
        // 准备时间步嵌入
        struct ggml_tensor * timestep = create_timestep_tensor(t);
        
        // Classifier-free guidance
        // noise_pred = uncond_pred + guidance_scale * (cond_pred - uncond_pred)
        
        // UNet 前向传播
        struct ggml_tensor * noise_pred_cond = unet_forward(
            ctx, model_.unet, latent, timestep, text_emb, config_
        );
        
        struct ggml_tensor * noise_pred;
        if (uncond_emb && config_.guidance_scale > 1.0f) {
            struct ggml_tensor * noise_pred_uncond = unet_forward(
                ctx, model_.unet, latent, timestep, uncond_emb, config_
            );
            
            // CFG: pred = uncond + scale * (cond - uncond)
            noise_pred = ggml_add(
                ctx,
                noise_pred_uncond,
                ggml_scale(ctx, ggml_sub(ctx, noise_pred_cond, noise_pred_uncond), config_.guidance_scale)
            );
        } else {
            noise_pred = noise_pred_cond;
        }
        
        // Euler step
        latent = euler_step(ctx, latent, noise_pred, scheduler_, t, i);
    }
    
    return latent;
}

// Euler 采样器步骤
ggml_tensor * euler_step(
    ggml_context * ctx,
    ggml_tensor * latent,
    ggml_tensor * noise_pred,
    const Scheduler & scheduler,
    int t,
    int step_idx
) {
    float alpha = scheduler.get_alpha_cumprod(t);
    float sigma = sqrt(1.0f - alpha);
    
    // v-prediction model:
    // x_0 = sqrt(alpha) * latent - sqrt(1-alpha) * noise_pred
    // 或者对于 v-prediction: v = sqrt(alpha) * noise - sqrt(1-alpha) * x_0
    
    if (config_.v_prediction) {
        // V-prediction 转换
        // x_0 = sqrt(alpha) * latent - sqrt(1-alpha) * noise_pred
        float sqrt_alpha = sqrtf(alpha);
        float sqrt_one_minus_alpha = sqrtf(1.0f - alpha);
        
        struct ggml_tensor * x0 = ggml_sub(
            ctx,
            ggml_scale(ctx, latent, sqrt_alpha),
            ggml_scale(ctx, noise_pred, sqrt_one_minus_alpha)
        );
        
        // Euler 方法: x_{t-1} = x_t - sigma * noise_pred
        struct ggml_tensor * d = ggml_scale(ctx, noise_pred, sigma);
        
        // 下一步 (简化版，实际需要更多计算)
        if (step_idx < config_.sample_steps - 1) {
            int t_next = scheduler.timesteps[step_idx + 1];
            float sigma_next = sqrtf(1.0f - scheduler.get_alpha_cumprod(t_next));
            
            latent = ggml_sub(ctx, latent, ggml_scale(ctx, noise_pred, sigma - sigma_next));
        } else {
            latent = x0;
        }
    }
    
    return latent;
}

} // namespace sd
```

### 7.2 主程序入口

```cpp
// src/sd-main.cpp

#include "sd-pipeline.h"
#include <iostream>
#include <stb_image_write.h>  // 保存图像

void print_usage(const char * program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  -m, --model <path>     Path to GGUF model\n"
              << "  -p, --prompt <text>    Text prompt\n"
              << "  -n, --negative <text>  Negative prompt\n"
              << "  -o, --output <path>    Output image path\n"
              << "  -s, --steps <n>        Sampling steps (default: 20)\n"
              << "  -g, --guidance <f>     Guidance scale (default: 7.5)\n"
              << "  --seed <n>             Random seed\n"
              << "  --height <n>           Image height (default: 768)\n"
              << "  --width <n>            Image width (default: 768)\n";
}

int main(int argc, char ** argv) {
    std::string model_path;
    std::string prompt;
    std::string negative_prompt;
    std::string output_path = "output.png";
    int steps = 20;
    float guidance = 7.5f;
    int seed = -1;
    int height = 768;
    int width = 768;
    
    // 解析参数...
    
    if (model_path.empty() || prompt.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    
    // 配置
    sd::SDConfig config;
    config.sample_steps = steps;
    config.guidance_scale = guidance;
    config.seed = seed;
    config.unet_sample_size = height / 8;  // Latent size
    
    // 创建 pipeline
    std::cout << "Loading model from " << model_path << "...\n";
    sd::SDPipeline pipeline(model_path, config);
    
    // 生成图像
    std::cout << "Generating image for prompt: " << prompt << "\n";
    ggml_tensor * image = pipeline.text_to_image(prompt, negative_prompt, seed);
    
    // 保存图像
    std::cout << "Saving to " << output_path << "\n";
    save_image(image, output_path);
    
    return 0;
}

void save_image(ggml_tensor * image, const std::string & path) {
    int height = image->ne[1];
    int width = image->ne[0];
    int channels = image->ne[2];
    
    float * data = (float *)image->data;
    
    // 转换为 uint8
    std::vector<uint8_t> pixels(width * height * channels);
    for (int i = 0; i < width * height * channels; i++) {
        pixels[i] = std::clamp((int)(data[i] * 255), 0, 255);
    }
    
    stbi_write_png(path.c_str(), width, height, channels, pixels.data(), width * channels);
}
```

---

## 8. 性能优化策略

### 8.1 内存优化

```cpp
// 使用内存高效的注意力
struct ggml_tensor * memory_efficient_attention(
    struct ggml_context * ctx,
    struct ggml_tensor * q,
    struct ggml_tensor * k,
    struct ggml_tensor * v,
    int num_heads
) {
    // 使用 Flash Attention 或 Sliced Attention
    // 避免 O(n^2) 的注意力矩阵存储
    
    // 方法1: Flash Attention (如果 GGML 支持)
    return ggml_flash_attn_ext(ctx, q, k, v, nullptr, 1.0f, 0.0f, 0.0f);
    
    // 方法2: 分块注意力
    // 将 Q 分块，逐块计算注意力
}

// 降低精度
void optimize_for_memory(SDModel & model) {
    // 将权重转换为 Q8_0 或 Q4_K
    // 注意力计算使用 F32 保持精度
    
    // UNet 卷积: Q8_0 (平衡质量和大小)
    // 注意力权重: Q8_0
    // 时间嵌入: F16
}
```

### 8.2 计算优化

```cpp
// 使用 CUDA 加速
void setup_cuda_backend(SDModel & model) {
    // 创建 CUDA backend
    ggml_backend_t cuda_backend = ggml_backend_cuda_init(0);
    
    // 创建 scheduler 支持 GPU offload
    int n_backends = 2;
    ggml_backend_t backends[] = { cuda_backend, ggml_backend_cpu_init() };
    ggml_backend_buffer_type_t bufts[] = {
        ggml_backend_cuda_buffer_type(0),
        ggml_backend_cpu_buffer_type()
    };
    
    ggml_backend_sched_t sched = ggml_backend_sched_new(
        backends, bufts, n_backends,
        8192,  // graph size
        false, // not parallel
        true   // allow op offload
    );
}

// 使用 CUDA Graph 优化迭代循环
void optimize_sampling_loop() {
    // 预编译计算图
    // 重用相同的图结构
}
```

### 8.3 采样优化

```cpp
// DPM-Solver++: 更快的采样器
ggml_tensor * dpm_solver_plusplus_sample(
    ggml_context * ctx,
    SDPipeline & pipeline,
    ggml_tensor * text_emb,
    ggml_tensor * uncond_emb,
    int steps
) {
    // DPM-Solver++ 只需要 10-20 步
    // 比 Euler 更高效
    
    // 实现略...
}

// 使用 xFormers 风格的内存高效注意力
// 需要在 GGML 中实现对应算子
```

---

## 9. 测试与验证

### 9.1 算子单元测试

```cpp
// tests/test_ops.cpp

#include "ggml.h"
#include <cmath>
#include <cassert>

void test_group_norm() {
    ggml_init_params params = { 1024 * 1024, nullptr };
    ggml_context * ctx = ggml_init(params);
    
    // 创建测试数据
    int C = 320, H = 8, W = 8, B = 1;
    ggml_tensor * x = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, W, H, C, B);
    ggml_tensor * w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, C);
    ggml_tensor * b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, C);
    
    // 填充测试数据
    float * x_data = (float *)x->data;
    for (int i = 0; i < W * H * C * B; i++) {
        x_data[i] = (i % 100) / 100.0f;  // 0.0 - 1.0
    }
    
    // 计算 GroupNorm
    int num_groups = 32;
    ggml_tensor * y = ggml_group_norm(ctx, x, w, b, num_groups, 1e-5f);
    
    // 执行计算图
    ggml_cgraph * graph = ggml_new_graph(ctx);
    ggml_build_forward_expand(graph, y);
    ggml_graph_compute_with_ctx(ctx, graph, 1);
    
    // 验证结果
    float * y_data = (float *)y->data;
    // 检查每个 group 的均值和方差
    int group_size = C / num_groups;
    for (int g = 0; g < num_groups; g++) {
        float mean = 0, var = 0;
        for (int i = 0; i < group_size * H * W; i++) {
            mean += y_data[g * group_size * H * W + i];
        }
        mean /= (group_size * H * W);
        assert(std::abs(mean) < 1e-4);  // 均值应接近 0
    }
    
    std::cout << "GroupNorm test passed!\n";
}

void test_conv2d() {
    // 类似地测试 Conv2D
    // 验证输出尺寸和数值正确性
}

int main() {
    test_group_norm();
    test_conv2d();
    // ... 其他测试
    return 0;
}
```

### 9.2 集成测试

```cpp
// tests/test_clip.cpp

void test_clip_encoding() {
    // 加载小型 CLIP 模型进行测试
    // 对比 PyTorch 输出
}

void test_unet_denoise() {
    // 测试单步去噪
    // 验证输出形状和范围
}

void test_vae_decode() {
    // 测试 VAE 解码
    // 验证输出图像尺寸
}
```

### 9.3 与参考实现对比

```python
# tests/compare_with_torch.py

import torch
from diffusers import StableDiffusionPipeline
import numpy as np

def compare_outputs():
    # 使用相同的输入
    prompt = "a photo of a cat"
    seed = 42
    
    # PyTorch 生成
    pipe = StableDiffusionPipeline.from_pretrained("stabilityai/stable-diffusion-2-1")
    torch_image = pipe(prompt, generator=torch.Generator().manual_seed(seed)).images[0]
    
    # GGML 生成
    # ... 调用编译的可执行文件
    
    # 对比图像相似度
    # SSIM, PSNR 等指标
```

---

## 附录 A: CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(sd-inference CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GGML 路径
set(GGML_DIR ${CMAKE_SOURCE_DIR}/../llama.cpp)

# 添加 GGML
add_subdirectory(${GGML_DIR} ggml)

# 可执行文件
add_executable(sd-inference
    src/sd-main.cpp
    src/sd-common.cpp
    src/sd-clip.cpp
    src/sd-unet.cpp
    src/sd-vae.cpp
    src/sd-pipeline.cpp
)

target_include_directories(sd-inference PRIVATE
    ${GGML_DIR}/ggml/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(sd-inference PRIVATE
    ggml
    ggml-cpu
    # ggml-cuda  # 如果启用 CUDA
)

# 测试
enable_testing()
add_executable(test_ops tests/test_ops.cpp)
target_link_libraries(test_ops PRIVATE ggml)
add_test(NAME test_ops COMMAND test_ops)
```

---

## 附录 B: 开发检查清单

- [ ] **准备阶段**
  - [ ] 编译 GGML/llama.cpp 环境
  - [ ] 准备 SD2.x 原始模型权重
  - [ ] 安装 Python 转换工具依赖

- [ ] **模型转换**
  - [ ] 实现 GGUF 格式写入
  - [ ] 转换 CLIP 权重
  - [ ] 转换 UNet 权重
  - [ ] 转换 VAE 权重
  - [ ] 验证权重数值正确性

- [ ] **算子验证**
  - [ ] 测试 `ggml_group_norm`
  - [ ] 测试 `ggml_conv_2d`
  - [ ] 测试 `ggml_conv_transpose_2d`
  - [ ] 测试 `ggml_timestep_embedding`
  - [ ] 测试 Cross Attention 组合

- [ ] **组件实现**
  - [ ] CLIP Text Encoder
  - [ ] UNet Forward
  - [ ] VAE Decoder
  - [ ] Tokenizer

- [ ] **Pipeline 整合**
  - [ ] 文本编码流程
  - [ ] 扩散采样循环
  - [ ] VAE 解码
  - [ ] 图像保存

- [ ] **优化与测试**
  - [ ] 内存使用优化
  - [ ] CUDA 加速
  - [ ] 与参考实现对比
  - [ ] 质量验证

---

这份文档提供了基于 GGML 实现 Stable Diffusion 2.x 模型推理的完整指南。开发过程中需要逐步验证每个组件的正确性，并根据实际情况调整实现细节。
