# GGML Omni 多模态模型推理开发指南

## 目录

1. [Omni 模型概述](#1-omni-模型概述)
2. [支持的 Omni 模型类型](#2-支持的 omni-模型类型)
3. [模型架构详解](#3-模型架构详解)
4. [FFmpeg 音视频预处理](#4-ffmpeg-音视频预处理)
5. [GGUF 格式转换](#5-gguf-格式转换)
6. [核心组件实现](#6-核心组件实现)
7. [完整推理流程](#7-完整推理流程)
8. [实战示例](#8-实战示例)

---

## 1. Omni 模型概述

### 1.1 什么是 Omni 模型

Omni（全模态）模型是指能够同时处理**文本、图像、音频、视频**等多种输入模态的 AI 模型。与传统的单一模态模型相比，Omni 模型具有以下特点：

- **统一架构**: 使用单个模型处理多种输入
- **跨模态理解**: 能够理解不同模态之间的关联
- **任意组合**: 支持多种输入的组合（如文本 + 图像 + 音频）
- **原生支持**: 不是简单的多模型拼接，而是真正的端到端训练

### 1.2 典型应用场景

| 场景 | 输入模态 | 输出模态 | 示例 |
|------|----------|----------|------|
| 视频问答 | 视频 + 文本 | 文本 | "视频中的人在做什么？" |
| 语音对话 | 音频 + 文本 | 文本/音频 | 语音助手对话 |
| 多模态内容分析 | 图像 + 音频 + 文本 | 文本 | 分析带字幕的视频片段 |
| 实时翻译 | 音频 + 视频 | 文本 | 视频会议实时字幕 |
| 内容创作 | 文本 + 图像参考 | 图像/文本 | 根据描述生成配图文章 |

### 1.3 GGML 对 Omni 模型的支持

GGML/llama.cpp 已经支持多种 Omni 模型架构：

```python
# GGUF 中定义的 Omni 模型类型 (来自 gguf/constants.py)
ARCHITECTURE_MAP = {
    "Qwen2_5OmniModel": "qwen2.5o",      # Qwen2.5-Omni
    "MiniCPMO": "minicpmo",              # MiniCPM-O
    "Ultravox": "ultravox",              # Ultravox
    "Voxtral": "voxtral",                # Mistral Voxtral
    "LFM2": "lfm2",                      # LFM2 (Audio+Vision)
}
```

---

## 2. 支持的 Omni 模型类型

### 2.1 Qwen2.5-Omni 系列

**代表模型**: Qwen2.5-Omni-3B, Qwen2.5-Omni-7B

```yaml
架构特点:
  - Thinker 部分：基于 Qwen2.5 LLM
  - Vision Encoder: Qwen2-VL 视觉编码器
  - Audio Encoder: Whisper-style 音频编码器
  - 支持模态：文本 + 图像 + 音频
  
关键参数:
  hidden_size: 2048 (3B) / 4096 (7B)
  vision_hidden_size: 1536
  audio_hidden_size: 768
  max_sequence_length: 32768
  
GGUF 下载:
  - ggml-org/Qwen2.5-Omni-3B-GGUF
  - ggml-org/Qwen2.5-Omni-7B-GGUF
```

### 2.2 MiniCPM-O 系列

**代表模型**: MiniCPM-o-2_6

```yaml
架构特点:
  - 基于 MiniCPM-2.4B
  - 支持图像和视频理解
  - 高效的多模态融合机制
  
关键参数:
  hidden_size: 2304
  vision_encoder: CLIP ViT-L/14
  projector_type: mlp_2x
  
GGUF 下载:
  - openbmb/MiniCPM-o-2_6-gguf
```

### 2.3 Ultravox 系列

**代表模型**: Ultravox-v0_5-llama-3_2-1b

```yaml
架构特点:
  - 基于 Llama-3.2
  - 专注于音频 + 文本理解
  - 轻量级设计
  
关键参数:
  hidden_size: 2048
  audio_encoder: Whisper-medium
  projector_type: two_layer_mlp
  
GGUF 下载:
  - ggml-org/ultravox-v0_5-llama-3_2-1b-GGUF
```

### 2.4 Voxtral 系列

**代表模型**: Voxtral-Mini-3B-2507

```yaml
架构特点:
  - Mistral 家族的音频模型
  - 基于 Mistral-7B 架构
  - 优秀的音频理解能力
  
关键参数:
  hidden_size: 4096
  audio_encoder: Whisper-large-v3
  max_audio_length: 30s
  
GGUF 下载:
  - ggml-org/Voxtral-Mini-3B-2507-GGUF
```

---

## 3. 模型架构详解

### 3.1 通用 Omni 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Omni Model Architecture                   │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                │
│  │  Text    │   │  Image   │   │  Audio   │                │
│  │  Input   │   │  Input   │   │  Input   │                │
│  └────┬─────┘   └────┬─────┘   └────┬─────┘                │
│       │              │              │                       │
│       ▼              ▼              ▼                       │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                │
│  │  Token   │   │  Vision  │   │  Audio   │                │
│  │ Embedding│   │ Encoder  │   │ Encoder  │                │
│  └────┬─────┘   └────┬─────┘   └────┬─────┘                │
│       │              │              │                       │
│       └──────────────┼──────────────┘                       │
│                      │                                      │
│                      ▼                                      │
│               ┌─────────────┐                               │
│               │   Modality  │                               │
│               │   Projector │                               │
│               └──────┬──────┘                               │
│                      │                                      │
│                      ▼                                      │
│               ┌─────────────┐                               │
│               │     LLM     │                               │
│               │  (Thinker)  │                               │
│               └──────┬──────┘                               │
│                      │                                      │
│                      ▼                                      │
│               ┌─────────────┐                               │
│               │   Output    │                               │
│               │   Head      │                               │
│               └─────────────┘                               │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Vision Encoder 实现

Vision Encoder 负责将图像转换为模型可理解的 embedding 向量。

**核心步骤**:
1. Patch Embedding: 将图像分割为固定大小的 patches
2. Position Embedding: 添加位置信息
3. Transformer Layers: 多层自注意力机制提取特征
4. Projector: 映射到 LLM embedding 空间

详细代码实现请参考前面的技术文档。

### 3.3 Audio Encoder 实现

Audio Encoder 负责将音频波形或语谱图转换为 embedding 向量。

**核心步骤**:
1. Convolutional Frontend: 提取局部音频特征
2. Positional Embedding: 添加时间位置信息
3. Transformer Layers: 捕捉长距离依赖关系
4. Adapter: 投影到统一维度

### 3.4 Multi-modal Projector

Projector 是连接不同模态编码器和 LLM 的桥梁，常见类型有：

- **Linear**: 单层线性变换，简单高效
- **MLP 2X**: 两层 MLP，更强的表达能力
- **Cross Attention**: 使用交叉注意力机制，更灵活的特征融合

---

## 4. FFmpeg 音视频预处理

### 4.1 FFmpeg 安装

```bash
# Ubuntu/Debian
sudo apt update && sudo apt install ffmpeg

# macOS
brew install ffmpeg

# CentOS/RHEL
sudo yum install ffmpeg
```

### 4.2 视频处理命令

#### 4.2.1 提取视频帧

```bash
# 每秒提取 1 帧
ffmpeg -i input_video.mp4 -vf "fps=1" frames/frame_%03d.jpg

# 均匀提取 8 帧
ffmpeg -i input_video.mp4 -vf "select='not(mod(n,100))',scale=512:512" \
       -vframes 8 frames/frame_%03d.jpg

# 提取关键帧
ffmpeg -i input_video.mp4 -vf "select='eq(pict_type,I)'" -vsync vfr \
       frames/keyframe_%03d.jpg
```

#### 4.2.2 获取视频信息

```bash
# JSON 格式输出
ffprobe -v quiet -print_format json -show_format -show_streams input_video.mp4
```

### 4.3 音频处理命令

#### 4.3.1 音频提取

```bash
# 提取并重采样到 16kHz 单声道
ffmpeg -i input_video.mp4 -vn -acodec pcm_s16le -ar 16000 -ac 1 audio.wav

# 提取特定时间段
ffmpeg -i input_video.mp4 -ss 00:00:10 -t 00:00:30 -vn \
       -acodec pcm_s16le -ar 16000 -ac 1 clip.wav
```

### 4.4 Python 集成脚本

```python
import subprocess
from pathlib import Path

def extract_video_frames(video_path: str, output_dir: str, fps: float = 1.0):
    """从视频中提取帧"""
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    
    cmd = [
        'ffmpeg', '-i', video_path,
        '-vf', f'fps={fps}',
        f'{output_dir}/frame_%04d.jpg',
        '-y'
    ]
    
    subprocess.run(cmd, check=True, capture_output=True)
    return sorted(Path(output_dir).glob('frame_*.jpg'))

def extract_audio(audio_path: str, video_path: str, sample_rate: int = 16000):
    """从视频中提取音频"""
    cmd = [
        'ffmpeg', '-i', video_path,
        '-vn', '-acodec', 'pcm_s16le',
        '-ar', str(sample_rate), '-ac', '1',
        audio_path, '-y'
    ]
    
    subprocess.run(cmd, check=True, capture_output=True)
    return audio_path
```

---

## 5. GGUF 格式转换

### 5.1 环境准备

```bash
git clone https://github.com/ggerganov/llama.cpp.git
cd llama.cpp

pip install torch transformers safetensors sentencepiece protobuf gguf
```

### 5.2 Qwen2.5-Omni 转换脚本

```python
from transformers import AutoProcessor, Qwen2_5OmniForConditionalGeneration
from gguf import GGUFWriter, GGMLQuantizationType

def convert_qwen25_omni_to_gguf(model_path: str, output_path: str, quantize: bool = False):
    """转换 Qwen2.5-Omni 模型到 GGUF 格式"""
    
    # 加载模型
    processor = AutoProcessor.from_pretrained(model_path, trust_remote_code=True)
    model = Qwen2_5OmniForConditionalGeneration.from_pretrained(
        model_path,
        torch_dtype=torch.float16,
        device_map="cpu",
        trust_remote_code=True
    )
    
    # 创建 GGUF Writer
    writer = GGUFWriter(output_path, "qwen2.5-omni")
    
    # 添加元数据
    hparams = model.config.thinker_config.to_dict()
    writer.add_string("general.architecture", "qwen2.5o")
    writer.add_uint32("llama.embedding_length", hparams["hidden_size"])
    writer.add_uint32("llama.block_count", hparams["num_hidden_layers"])
    
    # Vision 参数
    vision_config = hparams.get("vision_config", {})
    if vision_config:
        writer.add_bool("qwen2.5o.has_vision_encoder", True)
        writer.add_uint32("qwen2.5o.vision_embedding_length", vision_config.get("hidden_size"))
    
    # Audio 参数
    audio_config = hparams.get("audio_config", {})
    if audio_config:
        writer.add_bool("qwen2.5o.has_audio_encoder", True)
        writer.add_uint32("qwen2.5o.audio_embedding_length", audio_config.get("d_model"))
    
    # 写入权重
    write_llm_weights(writer, model.thinker, quantize)
    write_vision_weights(writer, model.visual, quantize)
    write_audio_weights(writer, model.audio, quantize)
    write_projector_weights(writer, model.merger, quantize)
    
    # 保存文件
    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()
    
    print(f"Conversion complete! Output: {output_path}")
```

### 5.3 量化

```python
def quantize_gguf(input_path: str, output_path: str, quant_type: str = "Q4_K_M"):
    """量化 GGUF 模型"""
    from llama_cpp.llama import llama_model_quantize
    
    params = llama_model_quantize_params()
    params.nthread = 8
    params.type = {"Q4_K_M": 13, "Q8_0": 7, "Q5_K_M": 15}.get(quant_type, 13)
    
    llama_model_quantize(input_path.encode(), output_path.encode(), params)
    print(f"Quantized model saved to {output_path}")
```

---

## 6. 核心组件实现

### 6.1 数据结构定义

```cpp
// src/omni-common.h
#pragma once

#include "ggml.h"
#include <vector>
#include <string>

namespace omni {

struct OmniConfig {
    // LLM 参数
    int32_t hidden_size = 2048;
    int32_t num_hidden_layers = 24;
    int32_t num_attention_heads = 16;
    
    // Vision 参数
    bool has_vision = true;
    int32_t vision_hidden_size = 1536;
    int32_t vision_patch_size = 14;
    
    // Audio 参数
    bool has_audio = true;
    int32_t audio_hidden_size = 768;
    int32_t audio_num_mel_bins = 128;
};

struct MultimodalInput {
    std::vector<int32_t> tokens;
    std::vector<std::string> image_paths;
    std::string audio_path;
    std::vector<std::string> video_frames;
};

struct OmniModel {
    ggml_backend_t backend;
    ggml_context * ctx;
    
    // LLM 权重
    struct llm_weights_t {
        ggml_tensor * token_embeddings;
        std::vector<layer_t> layers;
        ggml_tensor * output_norm_weight;
    } llm;
    
    // Vision 权重
    struct vision_weights_t {
        ggml_tensor * patch_embeddings;
        ggml_tensor * position_embeddings;
        std::vector<layer_t> layers;
        ggml_tensor * merger_weight;
    } vision;
    
    // Audio 权重
    struct audio_weights_t {
        ggml_tensor * conv1_weight;
        ggml_tensor * conv2_weight;
        std::vector<layer_t> layers;
        ggml_tensor * adapter_weight;
    } audio;
};

} // namespace omni
```

### 6.2 Vision Encoder

```cpp
ggml_tensor * encode_image(
    ggml_context * ctx,
    const vision_weights_t & weights,
    ggml_tensor * images,
    const OmniConfig & config
) {
    // 1. Patch Embedding
    ggml_tensor * patches = ggml_conv_2d(
        ctx, weights.patch_embeddings, images,
        config.vision_patch_size, config.vision_patch_size,
        0, 0, 1, 1
    );
    
    // 2. Reshape & Add Position
    patches = ggml_reshape_3d(ctx, patches, 
                              config.vision_hidden_size,
                              n_patches, batch_size);
    patches = ggml_add(ctx, patches, weights.position_embeddings);
    
    // 3. Transformer Layers
    for (int i = 0; i < config.vision_num_layers; i++) {
        // Self-Attention + MLP
        // ... 详细实现见前文
    }
    
    // 4. Projector
    return ggml_mul_mat(ctx, weights.merger_weight, patches);
}
```

### 6.3 Audio Encoder

```cpp
ggml_tensor * encode_audio(
    ggml_context * ctx,
    const audio_weights_t & weights,
    ggml_tensor * mel_spectrogram,
    const OmniConfig & config
) {
    // 1. Convolutional Frontend
    ggml_tensor * features = ggml_conv_1d(ctx, weights.conv1_weight, mel_spectrogram, ...);
    features = ggml_gelu(ctx, features);
    features = ggml_conv_1d(ctx, weights.conv2_weight, features, ...);
    features = ggml_gelu(ctx, features);
    
    // 2. Transformer Layers
    // ...
    
    // 3. Adapter
    return ggml_mul_mat(ctx, weights.adapter_weight, features);
}
```

---

## 7. 完整推理流程

### 7.1 推理步骤

```
1. 加载 GGUF 模型 → OmniContext.initialize()
2. 预处理输入 → process_multimodal_input()
   - 文本 → Tokenize
   - 图像 → Resize → Normalize → Tensor
   - 音频 → Mel Spectrogram → Tensor
   - 视频 → Extract Frames → 多张图像
3. 编码多模态特征
   - encode_image() → Vision Embeddings
   - encode_audio() → Audio Embeddings
4. 构建完整输入嵌入
   - Concatenate: [Text] + [Vision] + [Audio]
5. LLM 自回归生成
   - Forward Pass → Logits → Sample → Next Token
6. 解码输出 → Detokenize → 文本响应
```

### 7.2 主程序

```cpp
int main(int argc, char ** argv) {
    // 1. 解析参数
    std::string model_path = "qwen2.5-omni-3b-q4_k_m.gguf";
    std::string prompt = "描述这张图片";
    std::vector<std::string> images = {"image1.jpg"};
    
    // 2. 初始化模型
    omni::OmniContext ctx;
    ctx.initialize(model_path);
    
    // 3. 准备输入
    omni::MultimodalInput input;
    input.text = prompt;
    input.image_paths = images;
    
    // 4. 生成响应
    auto tokens = ctx.generate(input, 256, 0.7f, 0.9f);
    
    // 5. 输出结果
    std::cout << decode_tokens(tokens) << std::endl;
    
    return 0;
}
```

---

## 8. 实战示例

### 8.1 视频问答

**任务**: 分析视频内容并回答问题

```bash
# 编译程序
cd llama.cpp/build
cmake .. -DGGML_CUDA=ON
make -j$(nproc) omni-inference

# 运行视频问答
./bin/omni-inference \
  -m qwen2.5-omni-3b-q4_k_m.gguf \
  -v input_video.mp4 \
  -p "视频中发生了什么？请用中文描述" \
  --fps 1 \
  -t 512
```

**预期输出**:
```
视频中展示了一位厨师在厨房里烹饪的场景。他正在用炒锅翻炒蔬菜，
动作熟练专业。厨房环境整洁明亮，可以看到各种调料和厨具摆放整齐...
```

### 8.2 音频转录 + 分析

**任务**: 转录音频并提供摘要

```bash
./bin/omni-inference \
  -m voxtral-mini-3b-q4_k_m.gguf \
  -a meeting_recording.wav \
  -p "请总结这段会议录音的主要内容" \
  -t 256
```

### 8.3 多图理解

**任务**: 分析多张图片的关系

```bash
./bin/omni-inference \
  -m minicpm-o-2_6-q4_k_m.gguf \
  -i frame_001.jpg -i frame_002.jpg -i frame_003.jpg \
  -p "这几张图片展示了什么故事？按顺序描述" \
  -t 512
```

---

## 附录 A: CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(omni-inference CXX)

set(CMAKE_CXX_STANDARD 17)

# GGML 路径
set(GGML_DIR ${CMAKE_SOURCE_DIR}/../llama.cpp)
add_subdirectory(${GGML_DIR} ggml)

# 可执行文件
add_executable(omni-inference
    src/omni-main.cpp
    src/omni-context.cpp
    src/omni-vision.cpp
    src/omni-audio.cpp
    src/omni-llm.cpp
)

target_include_directories(omni-inference PRIVATE
    ${GGML_DIR}/ggml/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(omni-inference PRIVATE
    ggml
    ggml-cpu
    ggml-cuda  # 可选
)
```

---

## 附录 B: 常见问题

**Q1: 内存不足怎么办？**
- 使用量化模型 (Q4_K_M, Q8_0)
- 减少 batch size
- 降低图像分辨率
- 缩短音频长度

**Q2: 推理速度慢？**
- 启用 CUDA/Metal 加速
- 使用 Flash Attention
- 减少生成的 token 数量
- 优化 FFmpeg 预处理参数

**Q3: 如何自定义模型？**
- 修改 GGUF 转换脚本适配新架构
- 调整 OmniConfig 参数
- 实现对应的 encoder 函数

---

这份文档提供了从零开始理解和实现 Omni 多模态模型推理的完整指南，涵盖了理论、工具、代码和实践各个方面。
