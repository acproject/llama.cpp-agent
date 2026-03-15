# llama.cpp-agent SDK 协议草图（HTTP 版 agent loop）

本 SDK 的目标是让 llama-server 保持无状态（HTTP 层不保存会话），同时把 agent\_loop、tools、permissions、subagent 等能力下沉到客户端，以 SDK 形式在不同语言复用。

## 1. 总体架构

- llama-server：只提供 OpenAI 兼容的 `/v1/chat/completions`（可流式）推理能力；不保存客户端会话。
- SDK（客户端）：维护会话（messages）、管理工具（tools）、执行工具并把 tool 结果回填到 messages，形成“HTTP 版 agent loop”闭环。

SDK 的核心是一个循环：

1. 发送 `messages + tools` 到 `/v1/chat/completions`
2. 收到 assistant 的 `content / reasoning_content / tool_calls`
3. 若存在 `tool_calls`：按权限策略执行本地工具，将结果以 `role=tool` 消息追加到 `messages`
4. 重复直到完成或达到迭代上限

## 2. 与 llama-server 的 HTTP 协议

### 2.1 Endpoint

- `POST /v1/chat/completions`

要求 llama-server 启用 `--jinja`，否则请求体里的 `tools / tool_choice` 会被拒绝（llama.cpp server 的约束）。

### 2.2 Request Body（OAI ChatCompletions）

最小请求体：

```json
{
  "model": "your-model-id",
  "messages": [],
  "tools": [],
  "tool_choice": "auto",
  "stream": true
}
```

- `messages`：OpenAI 兼容消息数组（见 3.1）。
- `tools`：函数工具列表（见 3.2）。SDK 会按“允许工具集合”对其过滤。
- `tool_choice`：建议默认 `"auto"`。
- `stream`：
  - `true`：服务端以 SSE 返回增量 chunk（见 2.3），SDK 将其映射为事件流（见 4）。
  - `false`：服务端返回一次性 JSON，SDK 直接解析完整 assistant 消息。

### 2.3 Streaming Response（SSE）

llama-server 的 SSE 数据行格式固定为：

- `data: <JSON>\n\n`
- 流结束时：`data: [DONE]\n\n`

每个 `<JSON>` 为一个 “chat.completion.chunk”，其关键增量字段位于：

- `choices[0].delta.content`：文本增量
- `choices[0].delta.reasoning_content`：思维增量（llama.cpp 扩展字段名）
- `choices[0].delta.tool_calls`：工具调用增量（可能跨多条 chunk 拼接）

SDK 的拼接规则：

- `content` / `reasoning_content`：按顺序追加到当前 assistant 消息缓冲。
- `tool_calls`：按 `tool_calls[].index` 聚合；同一 index 的 `function.arguments` 以字符串方式增量拼接；`function.name`、`id` 若出现则缓存；直到流结束或 `finish_reason` 指示结束后，形成完整 tool call 列表。

## 3. SDK 会话数据与工具协议

### 3.1 Messages（会话历史）

SDK 在内存里维护 OAI 兼容消息数组，并在每次 HTTP 推理请求时原样发送。

#### 3.1.1 user message

```json
{ "role": "user", "content": "..." }
```

#### 3.1.2 assistant message（含 tool\_calls）

```json
{
  "role": "assistant",
  "content": "...",
  "tool_calls": [
    {
      "id": "call_xxx",
      "type": "function",
      "function": { "name": "read", "arguments": "{\"file_path\":\"...\"}" }
    }
  ]
}
```

- `tool_calls[].function.arguments` 是字符串（JSON 字符串），SDK 执行前需要 `parse`。

#### 3.1.3 tool result message

```json
{
  "role": "tool",
  "tool_call_id": "call_xxx",
  "name": "read",
  "content": "tool output text"
}
```

SDK 在完成工具执行后，把工具输出回填为 `role=tool` 消息，作为下一轮推理输入的一部分。

### 3.2 Tools（函数工具定义）

SDK 侧工具定义需要能映射到 OAI `tools` 数组项：

```json
{
  "type": "function",
  "function": {
    "name": "read",
    "description": "Read a file",
    "parameters": { "type": "object", "properties": { } }
  }
}
```

约束：

- `name` 必须唯一。
- `parameters` 必须是合法 JSON Schema object。
- SDK 可以基于“允许工具集合”对 tools 做过滤（subagent 模式必需）。

### 3.3 Tool schema 复用策略

本仓库现有 `tool_registry` 的 `tool_def.parameters` 是 JSON schema 字符串；SDK 复用该字段并解析为 JSON object，生成 `tools[].function.parameters`。

## 4. SDK 事件流（与现有 agent\_loop 对齐）

SDK 输出的事件流用于：

- CLI 实时展示
- 上层语言 SDK 绑定统一事件模型（流式 UI）
- 与权限交互对接

事件统一结构：

```json
{ "type": "TEXT_DELTA", "data": { ... } }
```

### 4.1 事件类型与 payload

- `TEXT_DELTA`：`{ "delta": "..." }`
- `REASONING_DELTA`：`{ "delta": "..." }`
- `ITERATION_START`：`{ "iteration": 1, "max_iterations": 50 }`
- `TOOL_START`：`{ "name": "read", "arguments": "{...}" }`
- `TOOL_RESULT`：`{ "name": "read", "success": true, "output": "...", "error": "", "elapsed_ms": 12 }`
- `PERMISSION_REQUIRED`：`{ "request_id": "perm_xxx", "tool": "write", "details": "{...}", "is_dangerous": true }`
- `PERMISSION_RESOLVED`：`{ "request_id": "perm_xxx", "allowed": true }`
- `COMPLETED`：`{ "stop_reason": "COMPLETED|MAX_ITERATIONS|USER_CANCELLED|AGENT_ERROR", "stats": { ... } }`
- `ERROR`：`{ "message": "..." }`

SDK 的 `TEXT_DELTA/REASONING_DELTA` 来自 llama-server 的 SSE chunk；其余事件来自 SDK 自身的控制流与工具执行。

## 5. 权限交互（Permissions）

SDK 复用仓库现有的 permission 语义：

- 对不同 tool 类型有默认策略（例如 FILE\_READ 允许，FILE\_WRITE 询问）。
- 支持 “本次允许/本次拒绝/会话内记住”。
- 支持 doom-loop 检测（重复相同调用达到阈值时拒绝）。
- 支持 working\_dir 沙箱：对 file 操作若路径越界，提升为 EXTERNAL\_DIR 权限请求。

权限交互接口（SDK 侧）：

- 当工具即将执行且权限为 ASK：
  - 发出 `PERMISSION_REQUIRED`
  - SDK 阻塞等待上层回调回应（或由上层调用 `respond_permission(request_id, allowed, scope)`）
  - 然后发出 `PERMISSION_RESOLVED` 并继续/中止执行

## 6. Working directory 沙箱

- SDK 在创建会话时指定 `working_dir`（默认 `"."`）。
- 对 `read/write/edit`：
  - 相对路径按 `working_dir` 解析。
  - 若绝对路径不在 `working_dir`（含其子目录），提升为 EXTERNAL\_DIR 权限请求。
  - 对敏感文件路径（如私钥、SSH、系统文件等）直接拒绝或强制 ASK（取决于 permission 策略实现）。

## 7. Subagent（作为配置模式）

subagent 不依赖 server 侧能力，本质是“另一个 SDK 会话”，仅改变配置：

- 更换 system prompt：以 `base_system_prompt` 为前缀，追加 `# Subagent Mode: <type>` 与行为指南（以增强前缀复用）。
- 限制 tools：按 subagent type 只暴露允许集合。
- 限制 bash：对 EXPLORE 等只读模式启用 allowlist patterns。
- 降低 max\_iterations。
- 关闭 skills/AGENTS.md 注入（避免重复注入与成本）。

对上层暴露的统一入口仍然是 `task` 工具：

- `task(subagent_type, prompt, run_in_background, resume)` 在 SDK 内部实现：
  - 同步：直接运行子会话并返回最终文本（作为 `task` 的工具输出）
  - 后台：在客户端创建 task\_id + 本地线程/协程；resume 时查询状态/获取结果

## 8. Session State（可选：跨进程/跨语言携带）

为了让其它语言 SDK 复用同一套状态机，建议 SDK 暴露一个可序列化的 session\_state：

```json
{
  "messages": [],
  "permission": {
    "session_overrides": {},
    "recent_calls": []
  },
  "stats": {}
}
```

其它语言 SDK 只要实现：

- messages 维护与发送
- SSE chunk 解析与 tool\_calls 聚合
- tool 执行与回填
- permission 流程

即可达到与 C++ SDK 参考实现一致的行为。

## 9. System prompt 构造 API

为了让其它语言 SDK 可以 1:1 复现 C++ SDK 的 system prompt 拼接逻辑，SDK 暴露了一个纯函数式的构造接口（输入 base prompt + working\_dir + 开关，输出最终 prompt 与注入段）。

参考实现：

- [prompt-builder.h](file:///root/workspace/cpp_projects/llama.cpp-agent/agent/sdk/prompt-builder.h)
- [prompt-builder.cpp](file:///root/workspace/cpp_projects/llama.cpp-agent/agent/sdk/prompt-builder.cpp)

API 语义：

- `default_config_dir()`：返回 `~/.llama-agent`（或 Windows 的 `%APPDATA%\\llama-agent`）
- `build_system_prompt(base_prompt, working_dir, options)`：
  - skills 搜索：`<working_dir>/.llama-agent/skills`、`<config_dir>/skills`、`extra_skills_paths`
  - agents-md 搜索：从 `working_dir` 到 git root，并可包含 `<config_dir>/AGENTS.md`
  - 输出：`system_prompt = base_prompt + skills_section + agents_md_section`（以空行分隔）

## 10. C++ SDK 用法（参考实现）

### 10.1 作为库调用（http\_agent\_session）

入口类型：

- [http-agent.h](file:///root/workspace/cpp_projects/llama.cpp-agent/agent/sdk/http-agent.h)

示例（伪代码）：

```cpp
llama_agent_sdk::http_server_config server;
server.base_url = "http://127.0.0.1:8080";

llama_agent_sdk::http_agent_config cfg;
cfg.model = "your-model-id";
cfg.working_dir = ".";
cfg.enable_skills = true;
cfg.enable_agents_md = true;
cfg.enable_mcp = true;

llama_agent_sdk::http_agent_session session(server, cfg);
auto res = session.run_streaming("write a patch", on_event);
```

### 10.2 命令行用法（llama-agent-sdk）

可执行文件：`build/agent/llama-agent-sdk`

```bash
./build/agent/llama-agent-sdk --url http://127.0.0.1:8080 --model your-model-id --prompt "hello"
```

开关：

- `--no-skills`：禁用 skills 注入
- `--no-agents-md`：禁用 AGENTS.md 注入
- `--no-mcp`：禁用 MCP 启动与工具注册

## 11. llama-agent-server 用法（支持 Router mode 动态切换模型）

可执行文件：`build/agent/llama-agent-server`

### 11.1 单模型模式（MODEL mode）

启动时指定一个 GGUF 模型（行为接近 `llama-server -m ...`）：

```bash
./build/agent/llama-agent-server --host 127.0.0.1 --port 8080 -m /path/to/model.gguf
```

此模式下服务端只加载一个底座模型；如果要更换底座模型，需要重启进程并传入新的 `-m/--model`。

### 11.2 路由模式（ROUTER mode，运行中随时换模型）

启动时不指定 `-m/--model`，服务端作为 “router” 进程运行，按请求里的 `model` 路由到对应的子 server 实例（与 `llama-server` 的 router mode 一致）：

```bash
./build/agent/llama-agent-server --host 127.0.0.1 --port 8080 --models-dir /path/to/gguf_dir
```

模型管理（与 `llama-server` 一致）：

```bash
curl -s http://127.0.0.1:8080/models

curl -s http://127.0.0.1:8080/models/load \
  -H 'Content-Type: application/json' \
  -d '{"model":"your-model-id"}'

curl -s http://127.0.0.1:8080/models/unload \
  -H 'Content-Type: application/json' \
  -d '{"model":"your-model-id"}'
```

客户端如何“切换模型”：

- Router mode 下，模型选择是“逐请求”的：下一次请求把 `model` 改成另一个模型 ID 即完成切换。
- router 进程本身不保存“当前模型”；是否命中某个模型完全取决于该请求里的 `model` 字段（或 `/v1/agent/*` 的 query/body）。
- 若目标模型尚未加载，先调用 `/models/load`，再发送推理/agent 请求。

推理请求（与 `llama-server` 一致，通过 body 的 `model` 选择模型）：

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "your-model-id",
    "messages": [{"role":"user","content":"hello"}]
  }'
```

示例：同一 client 进程连续切换两个模型：

```bash
curl -s http://127.0.0.1:8080/models/load \
  -H 'Content-Type: application/json' \
  -d '{"model":"model-a"}'

curl -s http://127.0.0.1:8080/models/load \
  -H 'Content-Type: application/json' \
  -d '{"model":"model-b"}'

curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"model-a","messages":[{"role":"user","content":"hello from A"}]}'

curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"model-b","messages":[{"role":"user","content":"hello from B"}]}'
```

Agent 路由（`/v1/agent/*`）：

- `POST`：推荐在 JSON body 里包含 `"model": "your-model-id"`（router 会据此路由到对应模型实例）
- `GET`：通过 query 传 `?model=your-model-id`

示例：

```bash
curl -s http://127.0.0.1:8080/v1/agent/session \
  -H 'Content-Type: application/json' \
  -d '{"model":"your-model-id","working_dir":"."}'

curl -s "http://127.0.0.1:8080/v1/agent/sessions?model=your-model-id"
```

## 12. 多语言 SDKs（SDKs/）

仓库根目录的 `SDKs/` 提供了多语言 SDK 的“基础包骨架”，目标是让你能快速在不同语言里起一个最小可用的 client，并复用本文描述的协议（messages / SSE / tool_calls 聚合）。

所有 SDK 的共同点：

- `baseUrl`/`base_url` 指向同一个服务端（单模型或 router 均可）。
- “切换模型”方式一致：下一次请求使用不同的 `model` 值即可。
- 目前基础包只覆盖最小的 ChatCompletions（含 stream）；完整 agent loop（tools/permissions/subagent）可以按本文 1～8 的协议继续扩展。

### 12.1 Python（SDKs/python）

路径：`SDKs/python/src/llama_agent_sdk`

示例：

```python
from llama_agent_sdk import HttpAgentConfig, HttpServerConfig, HttpAgentSession

server = HttpServerConfig(base_url="http://127.0.0.1:8080")
cfg = HttpAgentConfig(model="your-model-id", system_prompt="You are a helpful assistant.")
session = HttpAgentSession(server, cfg)

out = session.chat_completions("hello")
print(out)
```

### 12.2 TypeScript（SDKs/typescript）

路径：`SDKs/typescript/src/index.ts`

示例：

```ts
import { HttpAgentSession } from "./dist/index.js";

const session = new HttpAgentSession(
  { baseUrl: "http://127.0.0.1:8080" },
  { model: "your-model-id", systemPrompt: "You are a helpful assistant." }
);

const out = await session.chatCompletions("hello");
console.log(out);
```

构建：

```bash
cd SDKs/typescript
npm install
npm run build
```

### 12.3 Java（SDKs/java）

包名：`ai.llama.agent.sdk`

示例（伪代码）：

```java
var server = new HttpServerConfig("http://127.0.0.1:8080");
var cfg = new HttpAgentConfig("your-model-id", "You are a helpful assistant.", java.time.Duration.ofSeconds(300));
var session = new HttpAgentSession(server, cfg);
var out = session.chatCompletions("hello", null);
System.out.println(out.toString());
```

构建：

```bash
cd SDKs/java
mvn -DskipTests -Dmaven.repo.local=.m2 package
```

### 12.4 Go（SDKs/go）

包名：`llamaagentsdk`

示例（伪代码）：

```go
server := llamaagentsdk.HttpServerConfig{BaseURL: "http://127.0.0.1:8080"}
cfg := llamaagentsdk.HttpAgentConfig{Model: "your-model-id"}
session := llamaagentsdk.NewHttpAgentSession(server, cfg)
out, err := session.ChatCompletions(context.Background(), "hello", nil)
```

### 12.5 Rust（SDKs/rust）

crate：`llama_agent_sdk`

示例（伪代码）：

```rust
let server = HttpServerConfig { base_url: "http://127.0.0.1:8080".to_string(), api_key: None };
let cfg = HttpAgentConfig { model: "your-model-id".to_string(), system_prompt: None, request_timeout: std::time::Duration::from_secs(300) };
let mut session = HttpAgentSession::new(server, cfg)?;
let out = session.chat_completions("hello", None)?;
```
