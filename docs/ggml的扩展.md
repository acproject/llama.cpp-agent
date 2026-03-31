# ggml_extend.hpp 扩展功能研究

## 概述

`ggml_extend.hpp` 是 stable-diffusion.cpp 项目中对 ggml 库的重要扩展文件，提供了大量的高级操作和工具函数，用于支持稳定扩散模型的复杂计算需求。该文件在基础 ggml 操作之上构建了更高级的抽象，使得神经网络层的实现更加简洁和高效。

## 目录结构

1. [基础工具函数](#基础工具函数)
2. [张量代数扩展](#张量代数扩展)
3. [LoRA 支持](#lora-支持)
4. [图像处理操作](#图像处理操作)
5. [神经网络层封装](#神经网络层封装)
6. **注意力机制扩展**
7. [归一化操作](#归一化操作)
8. [激活函数](#激活函数)
9. [卷积操作扩展](#卷积操作扩展)
10. [运行时管理](#运行时管理)

---

## 基础工具函数

### 内存对齐工具

```cpp
align_up_offset(int n, int multiple)   // 计算对齐偏移量
align_up(int n, int multiple)          // 计算对齐后的值
```
用于内存对齐计算，确保数据结构的内存布局满足硬件要求。

#### 日志回调
```cpp
ggml_log_callback_default(ggml_log_level level, const char* text, void*)
```
提供统一的日志处理回调，支持 DEBUG、INFO、WARN、ERROR 四个级别。

### 张量代数扩展

#### 1. n-模张量 - 矩阵乘积 (n-mode Tensor-Matrix Product)
函数: ggml_ext_mul_n_mode

功能: 实现张量沿着指定维度与矩阵的乘积操作，这是张量分解中的核心运算。

数学原理:
```txt
输入：A[ne03, k, ne01, ne00], B[k, m]
输出：Result[ne03, m, ne01, ne00]
```

实现步骤:
置换: 将第 mode 维交换到第 0 维
展平: 将 4D 张量转换为 2D 矩阵
矩阵乘法: 执行标准的矩阵乘法
重塑: 恢复为 4D 张量
逆置换: 还原维度顺序
应用场景: Tucker 分解、高阶 SVD、张量神经网络


#### 2. Kronecker 积
函数: ggml_ext_kronecker

功能: 实现两个张量的 Kronecker 积（张量积）

数学表达:
```txt
[ne03,ne02,ne01,ne00] ⊗ [ne13,ne12,ne11,ne10] 
= [ne03*ne13, ne02*ne12, ne01*ne11, ne00*ne10]
```

实现方式: 通过插值和逐元素乘法实现
```cpp
result = interpolate(a) * b
```
应用: LoKr 分解、模型压缩

#### 张量拼接
函数: ggml_ext_tensor_concat

功能: 沿指定维度拼接两个张量

特点:
* 自动检查非拼接维度的一致性
* 支持任意维度（0-3）的拼接
* 手动实现而非依赖 ggml 原生操作

### LoRA 支持
#### 1. LoRA 权重合并
函数: ggml_ext_merge_lora

功能: 将 LoRA 的低秩权重合并回原始权重形状

支持的两种模式:

**模式 1: 全连接层 (无 lora_mid)**
```txt
ΔW = B × A
其中:
- A (lora_down): [rank, in_features]
- B (lora_up): [out_features, rank]
- ΔW: [out_features, in_features]
```

**模式 2: 卷积层 (使用 lora_mid - Tucker 分解)**
```txt
ΔW ≈ G ×₃ A ×₄ B
其中:
- G (lora_mid): [3, 3, rank, rank]
- A (lora_down): [rank, C_in, 1, 1]
- B (lora_up): [rank, C_out, 1, 1]
- ΔW: [3, 3, C_out, C_in]
```

实现细节:
* 自动处理任意维度的 LoRA 张量
* 通过 reshape 将高维张量展平为 2D
* 利用 n-模积实现卷积层的 Tucker 分解还原

#### 2. LoKr 前向传播
函数: ggml_ext_lokr_forward

功能: 实现 LoKr (Kronecker Product Decomposition) 的前向计算

核心思想: 使用 Kronecker 积分解进一步压缩参数量

```txt
W ≈ (W1 ⊗ W2) × input
```

支持的操作:
* 全连接层: 通过两次矩阵乘法实现
* 卷积层: 结合卷积操作的 LoKr 计算

优势: 相比标准 LoRA，LoKr 可以进一步减少参数量，特别适合大模型微调。

### 图像处理操作
#### 1. 图像 ↔ 张量转换
函数:
* ggml_tensor_to_sd_image: 将 ggml 张量转换为 SD 图像格式
* sd_image_to_ggml_tensor: 将 SD 图像转换为 ggml 张量
* sd_image_f32_to_ggml_tensor: 支持 float32 图像格式

特点:
* 自动处理数值范围缩放（[0,255] ↔ [0,1]）
* 支持多通道（RGB/RGBA）
* 支持视频帧（4D 张量）

#### 2. 分块处理 (Tiling)
函数:
* sd_tiling: 方形分块处理
* sd_tiling_non_square: 非方形分块处理
* sd_tiling_calc_tiles: 计算最优分块参数

功能: 将大图像分割成小块进行处理，解决显存不足问题

关键参数:
* tile_size: 分块大小
* tile_overlap_factor: 重叠比例（用于平滑融合）
* circular_x/y: 是否启用循环填充

融合算法: 使用 smootherstep 进行加权混合，避免块间边界
```cpp
smootherstep_f32(x) = x³(6x² - 15x + 10)  // 5 次多项式平滑
```

#### 3. 掩码处理
函数: ggml_ext_tensor_apply_mask

功能: 应用掩码进行图像修复（inpainting）

特性:
* 支持不同分辨率的掩码（自动缩放）
* 二值化处理（round）
* 掩码区域填充指定值（默认 0.5）

#### 4. 分块裁剪与合并
函数:
* ggml_ext_tensor_split_2d: 从大图中裁剪子块
* ggml_ext_tensor_merge_2d: 将子块合并回大图

支持:
* 循环填充（circular padding）
* 重叠区域混合
* 边界跳过（skip）


### 神经网络层封装
#### 面向对象的设计
文件定义了完整的神经网络层类层次结构：
```txt
GGMLBlock (基类)
├── UnaryBlock (单输入单输出层)
│   ├── Linear (全连接层)
│   ├── Embedding (嵌入层)
│   ├── Conv2d (2D 卷积)
│   ├── Conv3d (3D 卷积)
│   ├── LayerNorm (层归一化)
│   └── RMSNorm (均方根归一化)
├── GroupNorm (组归一化)
│   └── GroupNorm32 (32 组归一化)
└── MultiheadAttention (多头注意力)
```

#### 1. Linear (全连接层)

特性:
* 支持 bias 可选
* 强制 float32 精度选项
* 支持量化类型（Q4_0, Q5_0, Q8_0 等）
* 集成 LoRA 支持

优化:
```cpp
if (x->ne[2] * x->ne[3] > 1024) {
    // 大数据量时 reshape 以避免 CUDA 错误
}
```

#### 2. Conv2d (2D 卷积)
参数:
* stride, padding, dilation
* circular_x/y (循环填充)
* direct 模式（直接算法 vs 隐式算法）

特殊处理:
* 自动处理 kernel reshape
* 支持偏置项
* 集成 LoRA 的 Tucker 分解

#### 3. Conv3d (3D 卷积)
应用: 视频生成模型（如 Wan/Qwen）

实现: 通过组合 2D 卷积和 1D 卷积实现高效的 3D 卷积


#### 4. Embedding (嵌入层)
特性:
* 支持量化类型（仅 F16, Q8_0, Q5_1, Q50, Q41, Q4_0）
* 自动 batch 展开（解决 ggml batch 推理问题）


### 注意力机制扩展
#### 增强的注意力机制
函数: ggml_ext_attention_ext

功能: 实现了高度优化的多头注意力机制

关键特性:

1. Flash Attention 支持
```cpp
if (flash_attn && L_k % 256 != 0) {
    kv_pad = GGML_PAD(L_k, 256) - L_k;  // 对齐到 256
}
```
* 自动检测是否可以使用 Flash Attention
* 动态 padding 以匹配硬件要求
* 支持 mask 的 Flash Attention

2. KV Cache 缩放
```cpp
float kv_scale = 1.0f;  // 避免溢出
k = ggml_scale(ctx, k, kv_scale);
v = ggml_scale(ctx, v, kv_scale);
```

3. 多查询注意力 (MQA/GQA) 支持
```cpp
n_kv_head = k->ne[0] / d_head;  // 可独立于 q 的头数
```

4. 两种实现路径
Flash Attention 路径 (优先):
```cpp
kqv = ggml_flash_attn_ext(ctx, q, k, v, mask, scale, ...)
```

标准 Attention 路径 (fallback):
```cpp
kq = ggml_mul_mat(ctx, k, q);           // Q×K^T
kq = ggml_soft_max_inplace(ctx, kq);     // Softmax
kqv = ggml_mul_mat(ctx, v, kq);          // ×V
```

#### QKV 拆分工具
函数:
* split_qkv: 拆分序列数据的 QKV [N, L, 3*C] → (Q, K, V)
* split_image_qkv: 拆分图像数据的 QKV [N, 3*C, H, W] → (Q, K, V)

实现技巧: 使用 view 和 permute 避免数据复制


### 归一化操作
#### 1. Layer Normalization
函数: ggml_ext_layer_norm

公式
```txt
LN(x) = (x - mean) / std * weight + bias
```

特点:
* 可配置的 eps（默认 1e-5）
* 支持无 weight/bias 模式

#### 2. Group Normalization
函数: ggml_ext_group_norm / ggml_ext_group_norm_32

特点:
* 默认 32 组（SD/XL 标准配置）
* 自动 reshape weight/bias 为 4D
* eps = 1e-6（不同于 LN）

#### 3. RMS Normalization
函数: RMSNorm::forward

公式:
```txt
RMSNorm(x) = x / sqrt(mean(x²) + eps) * weight
```
应用: Transformer 架构（如 LLaMA、Wan）


### 激活函数
#### 封装的激活函数

```cpp
// SiLU (Swish) 线性变体
ggml_ext_silu_act(ctx, x, gate_first)

// GELU (Gaussian Error Linear Unit)
ggml_ext_gelu(ctx, x, inplace)

// Quick GELU (近似版本)
ggml_ext_gelu_quick(ctx, x, inplace)
```

#### SiLU 门控机制
功能: 实现 SwiGLU 等门控激活函数

流程:
1. 将输入沿 channel 维度分成两半
2. 一半经过 SiLU 激活
3. 两半相乘

```cpp
x = chunk(x, 2, dim=0)  # [N, C] → [N, C/2], [N, C/2]
gate = silu(x[0])
output = gate * x[1]
```

### 卷积操作扩展
#### 1. 增强的 2D 卷积

函数: ggml_ext_conv_2d

增强功能:
* 自动 scaling（用于量化）
* circular padding 支持
* 直接/间接算法选择
* 偏置项自动 reshape

#### 2. 3D 卷积封装

函数:
* ggml_ext_conv_3d: 标准 3D 卷积
* ggml_ext_conv_3d_nx1x1: 特殊的 Nx1x1 卷积（用于视频模型）

特性:
* 分离的 circular_x/circular_y 控制
* 智能处理 circular 与普通填充的组合

### 运行时管理

核心类: ``GGMLRunner``

功能: 提供完整的模型推理生命周期管理

#### 1. 多层级上下文管理
```cpp
params_ctx      // 参数张量上下文（可在 CPU）
compute_ctx     // 计算图上下文
cache_ctx       // 缓存张量上下文
offload_ctx     // 卸载中间上下文
```

#### 2. 内存管理

参数分配:
```cpp
alloc_params_buffer()     // 分配参数内存
free_params_buffer()      // 释放参数内存
get_params_buffer_size()  // 获取参数内存大小
```

计算图分配:
```cpp
alloc_compute_buffer()    // 分配计算缓冲区
free_compute_buffer()     // 释放计算缓冲区
```

#### 3. 异构计算支持
参数卸载:
```cpp
offload_params_to_runtime_backend()  // 参数从 CPU→GPU
offload_params_to_params_backend()   // 参数从 GPU→CPU
```

优势:
* 节省 GPU 显存
* 支持更大的模型
* 自动异步传输（CUDA/SYCL）

#### 4. 缓存系统
API:
```cpp
cache(name, tensor)              // 缓存张量
get_cache_tensor_by_name(name)   // 获取缓存张量
copy_cache_tensors_to_cache_buffer()  // 固化缓存
```

应用:
* Cross-attention 的 KV cache
* 文本编码结果缓存
* 时间步嵌入缓存

#### 5. 构建时张量
内置张量:
```cpp
one_tensor      // 值为 1 的张量
zero_int_tensor // 值为 0 的 int 张量
```

用途: 用于 graph 中的常量操作（如 scale、gather）

#### 6. 计算执行
完整流程:
```cpp
compute(
    get_graph,           // 构建计算图的回调
    n_threads,          // 线程数
    output,             // 输出张量
    output_ctx          // 输出上下文
)
```

内部步骤:
1. 卸载参数到运行时后端
2. 分配计算缓冲区
3. 重置计算上下文
4. 构建计算图
5. 分配图内存
6. 设置输入数据
7. 执行计算
8. 同步结果
9. 更新缓存
10. 清理资

#### 7. 后端感知优化
特性:
* CPU 后端：设置线程数
* GPU 后端：异步数据传输
* Vulkan 特殊处理：batch 限制
```cpp
if (!ggml_backend_is_cpu(runtime_backend)) {
    ggml_backend_tensor_get_async(...);
    ggml_backend_synchronize(...);
}
```

### WeightAdapter 模式
接口: ``WeightAdapter``

功能: 在运行时动态修改权重（用于 LoRA）

核心方法:
```cpp
patch_weight(...)           // 修补权重
forward_with_lora(...)      // 带 LoRA 的前向传播
get_extra_graph_size()      // 额外计算图大小
```

应用:
LoRA 微调
模型合并
实时权重调整

### 工具函数

#### 1. 张量操作工具
```cpp
// 张量迭代器
ggml_ext_tensor_iter(tensor, fn)

// 张量差异比较
ggml_ext_tensor_diff(a, b, gap)

// 统计计算
ggml_ext_tensor_mean(src)

// 原地操作
ggml_ext_tensor_add_inplace(a, b)
ggml_ext_tensor_scale_inplace(src, scale)
ggml_ext_tensor_clamp_inplace(src, min, max)
```

#### 2. 调试工具
```cpp
print_ggml_tensor(tensor, shape_only, mark)  // 打印张量信息
```

功能:
* 可选择只打印形状或完整数据
* 自动截断大张量（只显示前后 3 个元素）
* 支持 F32/F16/I32 类型

#### 3. 文件 I/O
```cpp
load_tensor_from_file(ctx, file_path)  // 从文件加载张量
// save_tensor_to_file(...)            // 保存到文件（已注释）
```

#### 4. 时间步嵌入
函数:
* timestep_embedding: 计算正弦位置嵌入
* set_timestep_embedding: 设置预计算的嵌入
* new_timestep_embedding: 创建新的嵌入张量
* ggml_ext_timestep_embedding: 增强版（支持 scaling）

公式:
```python
freqs[i] = exp(-log(max_period) * i / half)
embedding[j] = sin(t * freqs[j])
embedding[j + half] = cos(t * freqs[j])
```

应用: 扩散模型的时间步编码

#### 5. VAE 处理
```cpp
process_vae_input_tensor(src)   // [0,1] → [-1,1]
process_vae_output_tensor(src)  // [-1,1] → [0,1]
```

### 高级数据结构
#### GGMLBlock 系统
设计理念: 面向对象的神经网络层抽象

核心特性:

1. 层级化模块管理
```cpp
class GGMLBlock {
    GGMLBlockMap blocks;      // 子模块
    ParameterMap params;      // 参数
};
```

2. 自动参数初始化
```cpp
init(ctx, tensor_storage_map, prefix)
├── init_params()    // 初始化本层参数
└── init_blocks()    // 递归初始化子模块
```

3. 参数量统计
```cpp
get_params_num()   // 参数张量数量
get_params_mem_size()  // 参数内存占用
```

4. 类型推断
```cpp
get_type(name, tensor_storage_map, default_type)
```
根据 tensor_storage_map 自动推断参数类型（支持量化）

#### UnaryBlock 接口
定义: 单输入单输出的网络层

```cpp
class UnaryBlock : public GGMLBlock {
    virtual ggml_tensor* forward(ctx, x) = 0;
};
```

优势:
* 统一的前向传播接口
* 便于组合和链式调用
* 支持多态

### 性能优化技术
#### 1. 内存优化
视图而非复制
```cpp
ggml_view_4d(ctx, tensor, ...)  // 创建视图，不复制数据
```

延迟分配
```cpp
params.no_alloc = true  // 先创建图，后统一分配
```

内存复用
```cpp
reset_compute_ctx()     // 重用计算上下文
```

#### 2. 计算优化
就地操作
```cpp
ggml_scale_inplace(ctx, x, factor)
ggml_add_inplace(ctx, a, b)
```

精度控制
```cpp
ggml_mul_mat_set_prec(x, GGML_PREC_F32)  // 设置矩阵乘法精度
```

批量处理
```cpp
if (x->ne[2] * x->ne[3] > 1024) {
    reshape 以避免 CUDA 错误
}
```

#### 3. 并行化
多线程支持
```cpp
ggml_backend_cpu_set_n_threads(backend, n_threads)
```

异步操作
```cpp
ggml_backend_tensor_get_async(...)  // 异步数据传输
```

### 特殊技术支持
#### 1. 圆形填充 (Circular Padding)
应用: 无缝纹理生成、全景图像

实现:
```cpp
ggml_pad_ext_circular(ctx, x, lp0, rp0, ...)
```

示例:
```txt
输入：[H, W, C]
左填充 10: 从右侧取 10 列放到左侧
右填充 10: 从左侧取 10 列放到右侧
```

#### 2. 切片操作
函数: ggml_ext_slice

特性:
* 支持负索引（Python 风格）
* 可选连续性保证
* 零拷贝实现（使用 view）

示例:
```cpp
slice(x, dim=0, start=-10, end=None)  // 最后 10 个元素
```

#### 3. 分块操作
函数: ggml_ext_chunk

功能: 将张量沿指定维度均匀分割

示例:
```cpp
chunk(x, num=3, dim=2)  // [N, C, H, W] → 3 个 [N, C/3, H, W]
```

#### 4. Cast 操作
函数: ggml_ext_cast_f32

特殊处理: Vulkan 后端的类型转换 workaround

```cpp
#ifdef SD_USE_VULKAN
    // 使用 gather 实现 cast
    out = ggml_get_rows(ctx, out, zero_index);
#else
    // 使用矩阵乘法实现 cast
    out = ggml_mul_mat(ctx, one, out);
#endif
```

### 设计模式与最佳实践
#### 1. RAII 模式

所有资源管理类都遵循 RAII：
```cpp
GGMLRunner backend;  // 构造时初始化
// 自动使用
// 析构时自动释放
```

#### 2. 工厂模式
```cpp
ggml_ext_full(ctx, value, ne0, ne1, ne2, ne3)   // 创建填充张量
ggml_ext_zeros(ctx, ne0, ne1, ne2, ne3)         // 创建零张量
ggml_ext_ones(ctx, ne0, ne1, ne2, ne3)          // 创建一张量
```

#### 3. 策略模式
```cpp
struct WeightAdapter {
    // 不同的实现策略
    - LoRAWeightAdapter
    - LoKrWeightAdapter
    - MergeWeightAdapter
};
```

#### 4. 模板方法模式
```cpp
class GGMLBlock {
    virtual void init_params() = 0;  // 子类实现具体初始化
    void init() {                    // 固定流程
        init_params();
        init_blocks();
    }
};
```

### 与其他组件的集成
#### 1. 后端集成
支持所有 ggml 后端：
* CPU (ggml-cpu)
* CUDA (ggml-cuda)
* Metal (ggml-metal)
* Vulkan (ggml-vulkan)
* OpenCL (ggml-opencl)
* SYCL (ggml-sycl)

#### 2. 量化集成
支持的量化类型：
* F16/BF16（低精度浮点）
* Q4_0/Q4_1（4-bit 量化）
* Q5_0/Q5_1（5-bit 量化）
* Q8_0（8-bit 量化）

#### 3. 模型集成

直接支持以下模型架构：
* SD 1.x / 2.x
* SDXL
* SD3
* Wan / Qwen
* PhotoMaker
* ControlNet

### 核心贡献
1. 高阶张量操作: n-模积、Kronecker 积等
2. LoRA/LoKr完整支持: 从权重合并到前向传播
3. 图像处理工具链: 分块、掩码、转换
4. 神经网络层抽象: 面向对象的可组合层
5. 注意力机制优化: Flash Attention、KV Cache
6. 运行时管理框架: 内存、缓存、异构计算

### 设计亮点
* 类型安全: 编译期类型检查
* 零拷贝: 尽可能使用 view
* 可扩展: 易于添加新层和新操作
* 性能导向: 多种优化技术
* 易用性: 简洁的 API 设计