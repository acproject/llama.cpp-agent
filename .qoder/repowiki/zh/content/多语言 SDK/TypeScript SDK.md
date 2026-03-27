# TypeScript SDK 技术文档

<cite>
**本文档引用的文件**
- [SDKs/typescript/src/index.ts](file://SDKs/typescript/src/index.ts)
- [SDKs/typescript/src/minimemory.ts](file://SDKs/typescript/src/minimemory.ts)
- [SDKs/typescript/src/project-assistant.ts](file://SDKs/typescript/src/project-assistant.ts)
- [SDKs/typescript/package.json](file://SDKs/typescript/package.json)
- [SDKs/typescript/tsconfig.json](file://SDKs/typescript/tsconfig.json)
- [SDKs/typescript/dist/index.d.ts](file://SDKs/typescript/dist/index.d.ts)
- [SDKs/typescript/dist/minimemory.d.ts](file://SDKs/typescript/dist/minimemory.d.ts)
- [SDKs/typescript/dist/project-assistant.d.ts](file://SDKs/typescript/dist/project-assistant.d.ts)
</cite>

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构概览](#架构概览)
5. [详细组件分析](#详细组件分析)
6. [依赖关系分析](#依赖关系分析)
7. [性能考虑](#性能考虑)
8. [故障排除指南](#故障排除指南)
9. [结论](#结论)
10. [附录](#附录)

## 简介

本项目是一个基于 TypeScript 的 SDK，专为 llama.cpp-Agent 生态系统设计。该 SDK 提供了完整的 HTTP 客户端封装、MiniMemory 集成、项目助手工具等功能，支持异步编程模式和流式数据处理。SDK 架构采用模块化设计，包含类型安全的接口定义、错误处理机制和高性能的数据传输协议。

## 项目结构

TypeScript SDK 采用清晰的模块化组织结构，主要包含以下核心模块：

```mermaid
graph TB
subgraph "SDK 核心模块"
A[index.ts<br/>HTTP 客户端封装]
B[minimemory.ts<br/>MiniMemory 客户端]
C[project-assistant.ts<br/>项目助手工具]
end
subgraph "构建配置"
D[package.json<br/>包管理配置]
E[tsconfig.json<br/>TypeScript 配置]
end
subgraph "类型定义"
F[index.d.ts<br/>核心类型定义]
G[minimemory.d.ts<br/>MiniMemory 类型]
H[project-assistant.d.ts<br/>助手工具类型]
end
A --> F
B --> G
C --> A
C --> B
D --> E
```

**图表来源**
- [SDKs/typescript/src/index.ts:1-221](file://SDKs/typescript/src/index.ts#L1-L221)
- [SDKs/typescript/src/minimemory.ts:1-183](file://SDKs/typescript/src/minimemory.ts#L1-L183)
- [SDKs/typescript/src/project-assistant.ts:1-442](file://SDKs/typescript/src/project-assistant.ts#L1-L442)

**章节来源**
- [SDKs/typescript/package.json:1-18](file://SDKs/typescript/package.json#L1-L18)
- [SDKs/typescript/tsconfig.json:1-15](file://SDKs/typescript/tsconfig.json#L1-L15)

## 核心组件

### 类型定义系统

SDK 提供了完整的 TypeScript 类型定义，确保类型安全和开发体验：

- **基础类型**：`Json`、`ChatMessage`、`ToolCall`、`ChatCompletionChunk`
- **配置类型**：`HttpServerConfig`、`HttpAgentConfig`
- **响应类型**：`RespValue`（RESP 协议响应）

这些类型定义支持：
- 严格的参数验证
- 智能代码补全
- 编译时错误检测
- 文档生成

**章节来源**
- [SDKs/typescript/src/index.ts:1-51](file://SDKs/typescript/src/index.ts#L1-L51)
- [SDKs/typescript/dist/index.d.ts:1-45](file://SDKs/typescript/dist/index.d.ts#L1-L45)

### HTTP 客户端封装

`HttpAgentSession` 是 SDK 的核心组件，提供了完整的 HTTP 通信能力：

```mermaid
classDiagram
class HttpAgentSession {
-server : HttpServerConfig
-cfg : HttpAgentConfig
-_messages : ChatMessage[]
+constructor(server : HttpServerConfig, cfg : HttpAgentConfig)
+messages : ChatMessage[]
+clear() : void
+chatCompletions(userPrompt : string, extra? : Json) : Promise~Json~
+chatCompletionsStream(userPrompt : string, onDelta? : Function, extra? : Json) : Promise
-headers() : Record~string, string~
-throwHttpError(res : Response, url : string) : Promise~never~
-fetchChecked(url : string, init : RequestInit) : Promise~Response~
}
class HttpServerConfig {
+baseUrl : string
+apiKey? : string
}
class HttpAgentConfig {
+model : string
+workingDir? : string
+systemPrompt? : string
+requestTimeoutMs? : number
}
HttpAgentSession --> HttpServerConfig : "使用"
HttpAgentSession --> HttpAgentConfig : "使用"
```

**图表来源**
- [SDKs/typescript/src/index.ts:83-218](file://SDKs/typescript/src/index.ts#L83-L218)

**章节来源**
- [SDKs/typescript/src/index.ts:83-218](file://SDKs/typescript/src/index.ts#L83-L218)

## 架构概览

SDK 采用分层架构设计，支持多种使用场景：

```mermaid
graph TB
subgraph "应用层"
A[用户应用]
B[项目助手 CLI]
end
subgraph "SDK 层"
C[HttpAgentSession<br/>HTTP 客户端]
D[MiniMemoryClient<br/>内存数据库客户端]
E[工具函数<br/>工具调用处理]
end
subgraph "服务层"
F[llama.cpp-Agent 服务器]
G[MiniMemory 数据库]
H[外部服务<br/>Web 搜索/抓取]
end
A --> C
B --> C
B --> D
C --> F
D --> G
C --> H
E --> C
E --> D
```

**图表来源**
- [SDKs/typescript/src/project-assistant.ts:240-273](file://SDKs/typescript/src/project-assistant.ts#L240-L273)
- [SDKs/typescript/src/minimemory.ts:101-181](file://SDKs/typescript/src/minimemory.ts#L101-L181)

## 详细组件分析

### HttpAgentSession 组件

`HttpAgentSession` 提供了完整的聊天对话功能，支持同步和流式两种模式：

#### 同步对话流程

```mermaid
sequenceDiagram
participant Client as "客户端应用"
participant Session as "HttpAgentSession"
participant Server as "Agent 服务器"
Client->>Session : chatCompletions(prompt)
Session->>Session : 添加用户消息到消息列表
Session->>Server : POST /v1/chat/completions
Server-->>Session : JSON 响应
Session->>Session : 解析响应并添加助手消息
Session-->>Client : 返回完整响应
```

**图表来源**
- [SDKs/typescript/src/index.ts:139-155](file://SDKs/typescript/src/index.ts#L139-L155)

#### 流式对话处理

流式对话通过 Server-Sent Events (SSE) 实现实时数据传输：

```mermaid
sequenceDiagram
participant Client as "客户端应用"
participant Session as "HttpAgentSession"
participant Server as "Agent 服务器"
participant SSE as "SSE 流处理器"
Client->>Session : chatCompletionsStream(prompt)
Session->>Server : POST /v1/chat/completions (stream=true)
Server-->>Session : SSE 数据流
Session->>SSE : 解析数据块
loop 每个数据块
SSE->>Client : 调用回调函数
SSE->>Session : 累加内容和工具调用
end
Session->>Session : 组合完整消息
Session-->>Client : 返回最终结果
```

**图表来源**
- [SDKs/typescript/src/index.ts:157-217](file://SDKs/typescript/src/index.ts#L157-L217)

**章节来源**
- [SDKs/typescript/src/index.ts:139-217](file://SDKs/typescript/src/index.ts#L139-L217)

### MiniMemoryClient 组件

MiniMemoryClient 实现了完整的 Redis 兼容协议客户端：

```mermaid
classDiagram
class MiniMemoryClient {
-host : string
-port : number
-password? : string
-socket? : Socket
-buffer : Buffer
-pending : Pending[]
+constructor(opts : MiniMemoryOpts)
+connect() : Promise~void~
+close() : void
+command(args : string[]) : Promise~RespValue~
-onData(chunk : Buffer) : void
-onError(err : Error) : void
-onClose() : void
}
class RESPProtocol {
<<interface>>
+encodeCommand(args : string[]) : Buffer
+parseResp(buf : Buffer, start? : number) : ParsedResp
+respToJson(value : RespValue) : any
}
MiniMemoryClient --> RESPProtocol : "实现"
```

**图表来源**
- [SDKs/typescript/src/minimemory.ts:101-181](file://SDKs/typescript/src/minimemory.ts#L101-L181)

#### RESP 协议解析流程

```mermaid
flowchart TD
Start([开始解析]) --> CheckLead["检查首字节类型"]
CheckLead --> Simple{"简单字符串 (+)"}
CheckLead --> Error{"错误 (-)"}
CheckLead --> Int{"整数 (:)"}
CheckLead --> Bulk{"批量字符串 ($)"}
CheckLead --> Array{"数组 (*)"}
Simple --> ParseSimple["解析字符串行"]
Error --> ParseError["解析错误行"]
Int --> ParseInt["解析整数行"]
Bulk --> ParseBulk["解析批量数据"]
Array --> ParseArray["解析数组"]
ParseSimple --> ReturnVal["返回值对象"]
ParseError --> ReturnVal
ParseInt --> ReturnVal
ParseBulk --> ReturnVal
ParseArray --> ReturnVal
ReturnVal --> End([结束])
```

**图表来源**
- [SDKs/typescript/src/minimemory.ts:46-91](file://SDKs/typescript/src/minimemory.ts#L46-L91)

**章节来源**
- [SDKs/typescript/src/minimemory.ts:1-183](file://SDKs/typescript/src/minimemory.ts#L1-L183)

### 项目助手工具

项目助手工具集成了多个 AI 助手功能，包括 RAG 搜索、网络搜索、子代理执行等：

```mermaid
graph LR
subgraph "项目助手核心"
A[主循环 runAgentLoop]
B[工具注册]
C[消息处理]
end
subgraph "工具集"
D[RAG 搜索 rag_search]
E[任务执行 task]
F[网络搜索 web_search]
G[网页抓取 web_fetch]
end
subgraph "子代理"
H[Web 子代理]
I[Writer 子代理]
end
A --> B
A --> C
B --> D
B --> E
B --> F
B --> G
E --> H
E --> I
D --> MiniMemory[MiniMemoryClient]
F --> Web[Web 搜索]
G --> Web
```

**图表来源**
- [SDKs/typescript/src/project-assistant.ts:240-273](file://SDKs/typescript/src/project-assistant.ts#L240-L273)

**章节来源**
- [SDKs/typescript/src/project-assistant.ts:240-442](file://SDKs/typescript/src/project-assistant.ts#L240-L442)

## 依赖关系分析

### 模块依赖图

```mermaid
graph TB
subgraph "外部依赖"
A[Node.js 内置模块<br/>net, fs, os, path]
B[浏览器 API<br/>fetch, AbortSignal]
end
subgraph "内部模块"
C[index.ts<br/>核心导出]
D[minimemory.ts<br/>MiniMemory 客户端]
E[project-assistant.ts<br/>项目助手]
end
subgraph "类型定义"
F[index.d.ts<br/>核心类型]
G[minimemory.d.ts<br/>MiniMemory 类型]
H[project-assistant.d.ts<br/>助手类型]
end
C --> D
C --> E
E --> C
E --> D
F --> G
F --> H
A --> D
B --> C
B --> E
```

**图表来源**
- [SDKs/typescript/src/index.ts:1-221](file://SDKs/typescript/src/index.ts#L1-L221)
- [SDKs/typescript/src/minimemory.ts:1-21](file://SDKs/typescript/src/minimemory.ts#L1-L21)
- [SDKs/typescript/src/project-assistant.ts:1-6](file://SDKs/typescript/src/project-assistant.ts#L1-L6)

### 错误处理策略

SDK 实现了多层次的错误处理机制：

```mermaid
flowchart TD
Start([请求发起]) --> Fetch["HTTP 请求"]
Fetch --> Response{"响应状态"}
Response --> |成功| Parse["解析响应"]
Response --> |失败| HttpError["HTTP 错误处理"]
HttpError --> ThrowError["抛出错误"]
Parse --> Success["返回成功结果"]
ThrowError --> End([结束])
Success --> End
subgraph "错误分类"
A[网络错误]
B[HTTP 错误]
C[解析错误]
D[超时错误]
end
```

**图表来源**
- [SDKs/typescript/src/index.ts:112-137](file://SDKs/typescript/src/index.ts#L112-L137)

**章节来源**
- [SDKs/typescript/src/index.ts:112-137](file://SDKs/typescript/src/index.ts#L112-L137)

## 性能考虑

### 异步编程模式

SDK 采用现代异步编程模式，充分利用 JavaScript 的事件循环机制：

- **流式处理**：使用 `AsyncGenerator` 和 `ReadableStream` 实现高效的流式数据处理
- **Promise 链**：避免回调地狱，提高代码可读性和维护性
- **并发控制**：合理使用 `Promise.all` 和串行处理策略

### 内存管理

```mermaid
flowchart TD
Start([开始处理]) --> Buffer["缓冲区管理"]
Buffer --> Chunk["数据块处理"]
Chunk --> Accumulate["累积处理结果"]
Accumulate --> Cleanup["清理临时数据"]
Cleanup --> Next["下一块数据"]
Next --> Chunk
Next --> Done["处理完成"]
subgraph "内存优化策略"
A[流式处理减少内存占用]
B[及时清理缓冲区]
C[避免大对象复制]
D[使用迭代器模式]
end
```

**图表来源**
- [SDKs/typescript/src/index.ts:61-81](file://SDKs/typescript/src/index.ts#L61-L81)

### 网络优化

- **连接复用**：HTTP 客户端支持连接池和重用
- **超时控制**：内置请求超时机制，防止长时间阻塞
- **错误重试**：智能的错误检测和重试策略

## 故障排除指南

### 常见问题及解决方案

#### 连接问题

**问题**：无法连接到 Agent 服务器
**解决方案**：
1. 检查 `baseUrl` 配置是否正确
2. 验证网络连通性
3. 确认服务器端口开放

#### 认证失败

**问题**：API 密钥认证失败
**解决方案**：
1. 验证 `apiKey` 配置
2. 检查密钥格式和有效期
3. 确认服务器端配置

#### 超时问题

**问题**：请求超时或响应缓慢
**解决方案**：
1. 调整 `requestTimeoutMs` 参数
2. 检查服务器性能
3. 优化网络环境

#### 数据解析错误

**问题**：JSON 解析失败或数据格式不正确
**解决方案**：
1. 检查响应格式
2. 验证数据完整性
3. 使用类型守卫进行安全访问

**章节来源**
- [SDKs/typescript/src/index.ts:112-137](file://SDKs/typescript/src/index.ts#L112-L137)
- [SDKs/typescript/src/project-assistant.ts:148-159](file://SDKs/typescript/src/project-assistant.ts#L148-L159)

## 结论

TypeScript SDK 提供了一个功能完整、类型安全、性能优异的开发框架。其模块化设计使得开发者可以灵活地选择所需功能，同时保持代码的可维护性和扩展性。通过合理的错误处理机制和性能优化策略，SDK 能够满足生产环境的各种需求。

## 附录

### 安装和配置指南

#### 基础安装

```bash
npm install llama-agent-sdk-ts
```

#### TypeScript 配置

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "NodeNext",
    "moduleResolution": "NodeNext",
    "declaration": true,
    "strict": true,
    "skipLibCheck": true,
    "lib": ["ES2022", "DOM"],
    "types": ["node"]
  }
}
```

#### 基本使用示例

```typescript
import { HttpAgentSession } from 'llama-agent-sdk-ts';

const session = new HttpAgentSession(
  { baseUrl: 'http://localhost:8080', apiKey: 'your-api-key' },
  { model: 'your-model-name' }
);

// 同步对话
const response = await session.chatCompletions('Hello');

// 流式对话
const result = await session.chatCompletionsStream('Hello', (delta) => {
  console.log(delta);
});
```

### API 参考

#### HttpAgentSession 方法

| 方法名 | 参数 | 返回值 | 描述 |
|--------|------|--------|------|
| `chatCompletions` | `userPrompt: string, extra?: Json` | `Promise<Json>` | 发送同步对话请求 |
| `chatCompletionsStream` | `userPrompt: string, onDelta?: Function, extra?: Json` | `Promise<object>` | 发送流式对话请求 |
| `clear` | `void` | `void` | 清空消息历史 |

#### MiniMemoryClient 方法

| 方法名 | 参数 | 返回值 | 描述 |
|--------|------|--------|------|
| `connect` | `void` | `Promise<void>` | 建立连接 |
| `command` | `args: string[]` | `Promise<RespValue>` | 执行命令 |
| `close` | `void` | `void` | 关闭连接 |

### 最佳实践

1. **类型安全**：始终使用 TypeScript 类型定义
2. **错误处理**：实现完善的错误捕获和处理机制
3. **资源管理**：及时释放连接和内存资源
4. **性能优化**：合理使用流式处理和缓存策略
5. **测试覆盖**：编写全面的单元测试和集成测试