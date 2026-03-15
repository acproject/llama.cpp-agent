# MiniMemory / MiniCache Server

一个轻量级、类 Redis 的内存 KV 服务端实现，使用 RESP 协议收发命令，支持基础 KV、数据库选择、事务、过期时间、持久化（MCDB 快照 + AOF 追加日志）等。

## 目录结构

- 配置模板：`src/conf/mcs.conf`
- 测试用例参考：`docs/test.md`
- 构建产物（默认）：`build/bin/`
  - 服务端：`mini_cache_server(.exe)`
  - 客户端：`mini_cache_cli(.exe)`
  - 默认配置目录：`build/bin/conf/`

## 构建

### Windows（推荐）

```powershell
cmake -S . -B build
cmake --build build --config Release
```

构建完成后，可执行文件通常位于：

- `build/bin/mini_cache_server.exe`
- `build/bin/mini_cache_cli.exe`

### Linux / macOS

```bash
cmake -S . -B build
cmake --build build -j
```

## 配置说明

服务端默认读取 `conf/mcs.conf`，也可以通过命令行指定：

```bash
mini_cache_server --config conf/mcs.conf
```

### 常用配置项

以 `build/bin/conf/mcs.conf` 为例：

```conf
bind 0.0.0.0
port 6379
#requirepass password123

appendonly yes
appendfilename ./data/appendonly.aof
appendfsync everysec

save 1 1
save 15 10
save 60 1000

maxmemory 2gb
maxmemory-policy allkeys-lru
```

- `bind` / `port`：监听地址与端口
- `requirepass`：密码（当前工程对认证状态的校验逻辑较简化）
- `appendonly`：是否启用 AOF
- `appendfilename`：AOF 文件路径
  - 建议写相对路径（如 `./data/appendonly.aof`）
  - 程序会将相对路径解析为“相对配置文件所在目录”，避免工作目录变化导致找不到文件
- `save <seconds> <changes>`：MCDB 快照保存条件（类似 RDB）
- `maxmemory` / `maxmemory-policy`：内存限制与淘汰策略（策略实现依赖 DataStore 的具体逻辑）

### 持久化文件位置

当 `appendfilename` 使用相对路径时，AOF 最终会写到“配置文件所在目录”下。例如：

- 配置：`build/bin/conf/mcs.conf`
- `appendfilename ./data/appendonly.aof`
- 实际文件：`build/bin/conf/data/appendonly.aof`

同理，MCDB 快照文件默认在配置目录下：

- `build/bin/conf/dump.mcdb`

如果你看到 `build/bin/data/appendonly.aof` 或 `build/bin/dump.mcdb`，通常是旧版本/其他路径产生的文件，请以服务端启动时打印的 `Resolved data paths` 为准。

## 启动服务器

在构建目录下启动（推荐从 `build/bin` 目录运行）：

```powershell
cd build/bin
.\mini_cache_server.exe --config conf/mcs.conf
```

启动成功后会看到类似输出：

- `Server is listening on 0.0.0.0:6379`
- `MiniCache server started on 0.0.0.0:6379`

## 使用客户端连接与执行命令

```powershell
cd build/bin
.\mini_cache_cli.exe -h 127.0.0.1 -p 6379
```

进入交互界面后，命令示例（更多用例见 `docs/test.md`）：

```text
MC> PING
MC> SET mykey "Hello World"
MC> GET mykey
MC> EXISTS mykey
MC> DEL mykey
MC> GET mykey
```

### 持久化测试（参考）

```text
MC> SET persist_key "This will be saved"
MC> SAVE
# 重启服务器后
MC> GET persist_key
```

## 图谱与证据检索（结构化即推理）

本项目新增了一组图谱与证据检索增强命令，用于把“扩展邻居 / 枚举边 / 元字段”直接以结构化结果返回，减少上层二次查询（尤其适合 LLM 在结构层完成推理）。

### 数据模型（约定）

- 边（去重、可删）：`__edge:<from>:<rel>:<to>`（内部会对三段做百分号编码，外部直接用原始字符串即可）
- 元数据（通用）：`__meta:<subject>:<field> = <value>`
  - subject 可以是普通 key、节点 id（如 `topic:redis`）、或边 key（如 `__edge:topic%3Aredis:HAS_CHUNK:__chunk%3Adoc1%3A0`）
- 证据对象（EVIDENCE 系列约定）：
  - 原文：`OBJSET __chunk:<id> <mime> <data>` 存在 `__obj:__chunk:<id>:mime/data`
  - 向量：`SETNX __emb:<space>:<id> <f1> <f2> ...`（推荐把模型/维度写进 `<space>`，避免后续换模型导致混用；`<id>` 与 `__chunk:<id>` 一一对应）
- 兼容性：仍维护 `__graph:adj:<from>` 的旧字符串邻接表；查询优先走 `__edge:*`，找不到再回退到旧邻接表

### multilingual-e5-large-instruct（llama.cpp embedding）规范与示例

推荐在中文为主/中英混合语料中使用 `multilingual-e5-large-instruct` 作为统一 embedding space。该模型的典型检索范式是区分 query 与 passage 前缀：

- 文档/段落（passage）embedding：`passage: <chunk_text>`
- 查询（query）embedding：`query: <user_question>`

建议固定如下约定（全库一致）：

- 相似度：`cosine`
- 向量维度：`1024`（E5 large 系列常见配置；务必与写入的浮点数量一致）
- key 规范：`__emb:e5-multi-large-instruct_d1024_cosine:<id>`
  - 示例：`__emb:e5-multi-large-instruct_d1024_cosine:doc1:0` 对应 `__chunk:doc1:0`

端到端示例（外部生成 embedding -> 写入 MiniMemory -> 检索回证据）：

```text
# A) 写入 chunk 原文
MC> OBJSET __chunk:doc1:0 text/plain "本段介绍 Redis 的基本概念……"
MC> METASET __chunk:doc1:0 source wiki
MC> TAGADD __chunk:doc1:0 redis

# B) 用 llama.cpp 生成 passage embedding（示例程序名可能是 embedding / llama-embedding，以你的 llama.cpp 构建产物为准）
#    输入一定要带 passage: 前缀；输出 1024 维 float
#    ./embedding -m multilingual-e5-large-instruct-*.gguf -p "passage: 本段介绍 Redis 的基本概念……"
#
#    取输出向量（1024 个浮点）后，写入本服务端：
MC> SETNX __emb:e5-multi-large-instruct_d1024_cosine:doc1:0 <f1> <f2> ... <f1024>

# C) 建图：topic -> chunk（可选，用于 EVIDENCE.SEARCHF 候选集裁剪）
MC> METASET topic:redis type topic
MC> GRAPH.ADDEDGE topic:redis HAS_CHUNK __chunk:doc1:0

# D) 用 llama.cpp 生成 query embedding（输入一定要带 query: 前缀）
#    ./embedding -m multilingual-e5-large-instruct-*.gguf -p "query: Redis 是什么？"
#
#    将 query 向量用于检索（dim 必须写 1024，并提供 1024 个浮点）：
MC> EVIDENCE.SEARCHF 5 cosine 1024 <q1> <q2> ... <q1024> TAG redis META source wiki GRAPHFROM topic:redis GRAPHREL HAS_CHUNK GRAPHDEPTH 1
```

如果希望把 embedding 生成也集成到服务端（由服务端调用 llama.cpp），可在配置中启用并提供模型路径与 llama.cpp server：

- `embedding.enabled yes`
- `embedding.model_path <gguf>`
- `embedding.host <host>` / `embedding.port <port>`
- 可选：`embedding.llama_server <path>` + `embedding.autostart yes`（Linux 下可由服务端拉起 llama-server）

启用后可直接使用：

- `EMBED QUERY|PASSAGE <text...>`：返回向量（float 数组）
- `EMBED.SET <key> QUERY|PASSAGE <text...>`：生成向量并写入 `<key>`（等价于执行一次 `SETNX <key> ...`）

### 快速上手（示例）

```text
# 1) 写入一段 chunk（原文 + 向量 + 标签/元数据）
MC> OBJSET __chunk:doc1:0 text/plain "Redis 是一个内存数据库"
MC> SETNX __emb:demo_d3_cosine:doc1:0 0.10 0.20 0.30
MC> TAGADD __chunk:doc1:0 redis kv
MC> METASET __chunk:doc1:0 source wiki

# 2) 建图：把 topic 节点连接到 chunk
MC> METASET topic:redis type topic
MC> GRAPH.ADDEDGE topic:redis HAS_CHUNK __chunk:doc1:0
MC> GRAPH.EDGEPROP.SET topic:redis HAS_CHUNK __chunk:doc1:0 confidence 0.92

# 3) 一次性拿到 rel/to + 边属性 + from/edge/to 的关键元字段（固定槽位）
MC> GRAPH.NEIGHBORSX2 topic:redis HAS_CHUNK 10 \
    EDGE_METAKEYS 2 confidence source \
    FROM_METAKEYS 1 type \
    TO_METAKEYS 2 type source

# 4) 证据检索：按图谱结果限定候选集（只在 topic:redis 可达的 chunk/emb 中检索）
MC> EVIDENCE.SEARCHF 5 cosine 3 0.11 0.19 0.29 GRAPHFROM topic:redis GRAPHREL HAS_CHUNK GRAPHDEPTH 1 TAG redis
```

### 图谱命令（速查）

- `GRAPH.ADDEDGE <from> <rel> <to>`：新增/去重边
- `GRAPH.DELEDGE <from> <rel> <to>`：删边（同时删除该边所有 `__meta:<edge_key>:*`）
- `GRAPH.HASEDGE <from> <rel> <to>`：判断是否存在边（兼容旧邻接表）
- `GRAPH.EDGEPROP.SET <from> <rel> <to> <field> <value>`：设置边字段（存储为 `__meta:<edge_key>:<field>`）
- `GRAPH.EDGEPROP.GET <from> <rel> <to> [field]`：获取边字段
- `GRAPH.NEIGHBORS <node> [rel] [limit] [PROP <field> <value>]...`：邻居列表（`rel:to` 字符串数组）
- `GRAPH.NEIGHBORSX <node> [rel] [limit] [PROP ...] [EDGE_METAKEYS n ...] [FROM_METAKEYS n ...] [TO_METAKEYS n ...]`：结构化邻居（槽位数量随参数变化）
- `GRAPH.NEIGHBORSX2 <node> [rel] [limit] [PROP ...] [EDGE_METAKEYS n ...] [FROM_METAKEYS n ...] [TO_METAKEYS n ...]`：结构化邻居（固定 6 槽位）
- `GRAPH.EDGE.LIST <node> [rel] [limit] [PROP ...] [METAKEYS n ...]`：枚举从 node 出发的边（槽位数量随参数变化）
- `GRAPH.EDGE.LIST2 <node> [rel] [limit] [PROP ...] [EDGE_METAKEYS n ...] [FROM_METAKEYS n ...] [TO_METAKEYS n ...]`：枚举边（固定 7 槽位）
- `GRAPH.PATH <from> <to> [rel] [maxDepth] [PROP ...]`：最短路径（按边数，默认 maxDepth=4）

补充说明：

- `[rel] [limit]` 的解析规则：如果某个参数是“纯数字”，会被当作 limit；否则当作 rel
- `PROP <field> <value>`：按边字段做等值过滤（读取 `__meta:<edge_key>:<field>`）；如果当前节点只存在旧邻接表数据，无法进行 PROP 过滤

### 结构化输出（固定槽位版本）

- `GRAPH.NEIGHBORSX2`：每个邻居返回 6 个槽位 `[rel, to, edge_props, edge_meta, from_meta, to_meta]`
- `GRAPH.EDGE.LIST2`：每条边返回 7 个槽位 `[from, rel, to, edge_props, edge_meta, from_meta, to_meta]`
- 其中：
  - `edge_props`：该边当前存在的全部字段（来源于 `__meta:<edge_key>:*`，数量不固定）
  - `edge_meta/from_meta/to_meta`：按请求的 `*_METAKEYS` 返回固定字段列表；字段缺失返回 nil

### 与 EVIDENCE.SEARCHF 结合（候选集裁剪）

`EVIDENCE.SEARCHF <topk> <metric> <dim> <q1..qdim> [TAG t]... [META f v]... [KEYPREFIX p] [GRAPHFROM n] [GRAPHREL r] [GRAPHDEPTH d] [GRAPHCHAINLEN n rel1..reln]`

- `GRAPHFROM/GRAPHREL/GRAPHDEPTH`：从 `GRAPHFROM` 出发做 BFS 收集可达节点（可选 `GRAPHREL` 约束关系），把其中的 `__chunk:*` 或 `__emb:*` 映射为允许检索的 `__emb:*`
- `GRAPHCHAINLEN`：替代 BFS，显式指定多跳关系链（例如 `GRAPHCHAINLEN 2 HAS_DOC HAS_CHUNK`）
- `TAG`：要求 chunk 的 `__meta:__chunk:<id>:tags` 含指定 tag
- `META`：要求 chunk 的 `__meta:__chunk:<id>:<field> == <value>`
- `KEYPREFIX`：额外限制 embedding key 前缀（例如 `__emb:doc1:`）

## 在其他 C++ 项目中复用

本仓库不仅提供服务端/客户端可执行文件，也暴露了若干可复用的库目标（CMake）：

- `data_store`：核心存储引擎
- `resp_parser`：RESP 解析
- `command_handler`：命令处理（基于 DataStore）
- `config_parser`：配置解析
- `tcp_server`：TCP 服务器实现（依赖上述模块）

### 方式 A：作为子工程（推荐）

将本仓库作为子目录（或 git submodule）引入你的项目，例如目录结构：

```text
your_project/
  CMakeLists.txt
  third_party/
    MiniMemory/
```

在你的 `CMakeLists.txt` 中：

```cmake
add_subdirectory(third_party/MiniMemory)

add_executable(your_app main.cpp)
target_link_libraries(your_app PRIVATE data_store command_handler resp_parser)
target_include_directories(your_app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/third_party/MiniMemory/src/server
)
```

然后在代码里直接包含头文件，例如：

```cpp
#include "DataStore.hpp"
#include "CommandHandler.hpp"
```

### 方式 B：链接已构建产物

先在 MiniMemory 中完成构建（产物在 `build/bin`），然后在你的项目里：

- 头文件：添加 `MiniMemory/src/server` 到 include 路径
- 库文件：链接 `data_store` / `command_handler` / `resp_parser` 等对应的 `.lib/.dll`（Windows）或 `.so/.dylib`（Linux/macOS）
- 运行时：确保依赖的动态库与可执行文件在同一目录，或在系统库搜索路径中可见

## 协议兼容性

客户端与服务端通过 RESP 协议交互，命令格式与示例可参考 `docs/test.md`。

### 最小 RESP 驱动（A 档：只做协议层）

本服务端只要求客户端“按 RESP2 数组协议发命令，按 RESP2 解析返回”。不需要 Redis 语义兼容（集群、脚本、发布订阅等都不在范围内）。

关键约束：

- 仅支持数组形式命令：`*<n>\r\n$<len>\r\n<arg>\r\n...`（不支持 inline）
- 若配置了 `requirepass`，业务命令前必须先 `AUTH <password>`，否则会返回 `-NOAUTH ...`

下面给出各语言“单文件可复制”的最小驱动（connect/auth/call/close + RESP2 解析）。

#### Node.js（TypeScript/JavaScript）

```ts
import net from "net";

type Resp = string | number | null | Resp[] | { err: string };

function encode(args: (string | Buffer)[]): Buffer {
  const parts: Buffer[] = [];
  parts.push(Buffer.from(`*${args.length}\r\n`));
  for (const a of args) {
    const b = Buffer.isBuffer(a) ? a : Buffer.from(a);
    parts.push(Buffer.from(`$${b.length}\r\n`));
    parts.push(b);
    parts.push(Buffer.from("\r\n"));
  }
  return Buffer.concat(parts);
}

function parseLine(buf: Buffer, off: number): [string | null, number] {
  const idx = buf.indexOf("\r\n", off);
  if (idx < 0) return [null, off];
  return [buf.slice(off, idx).toString("utf8"), idx + 2];
}

function parseOne(buf: Buffer, off = 0): [Resp | null, number] {
  if (off >= buf.length) return [null, off];
  const t = String.fromCharCode(buf[off]);
  if (t === "+" || t === "-" || t === ":") {
    const [line, n] = parseLine(buf, off + 1);
    if (line === null) return [null, off];
    if (t === "+") return [line, n];
    if (t === "-") return [{ err: line }, n];
    return [Number(line), n];
  }
  if (t === "$") {
    const [line, n] = parseLine(buf, off + 1);
    if (line === null) return [null, off];
    const len = Number(line);
    if (len === -1) return [null, n];
    const need = n + len + 2;
    if (need > buf.length) return [null, off];
    const data = buf.slice(n, n + len).toString("utf8");
    return [data, need];
  }
  if (t === "*") {
    const [line, n] = parseLine(buf, off + 1);
    if (line === null) return [null, off];
    const cnt = Number(line);
    if (cnt === -1) return [null, n];
    let cur = n;
    const arr: Resp[] = [];
    for (let i = 0; i < cnt; i++) {
      const [v, next] = parseOne(buf, cur);
      if (v === null && next === cur) return [null, off];
      if (v === null) { arr.push(null); cur = next; continue; }
      arr.push(v);
      cur = next;
    }
    return [arr, cur];
  }
  return [{ err: "bad resp" }, buf.length];
}

export class MiniMemoryClient {
  private sock!: net.Socket;
  private rbuf = Buffer.alloc(0);

  async connect(host: string, port: number) {
    this.sock = net.createConnection({ host, port });
    this.sock.on("data", (d) => (this.rbuf = Buffer.concat([this.rbuf, d])));
    await new Promise<void>((res, rej) => {
      this.sock.once("connect", () => res());
      this.sock.once("error", rej);
    });
  }

  close() {
    if (this.sock) this.sock.end();
  }

  async call(args: string[]): Promise<Resp> {
    this.sock.write(encode(args));
    for (;;) {
      const [v, n] = parseOne(this.rbuf, 0);
      if (v !== null || n > 0) {
        this.rbuf = this.rbuf.slice(n);
        if (typeof v === "object" && v && "err" in v) throw new Error(v.err);
        return v;
      }
      await new Promise((r) => setTimeout(r, 1));
    }
  }

  async auth(password: string) {
    await this.call(["AUTH", password]);
  }
}
```

#### Python

```python
import socket

class RespError(Exception):
    pass

def encode(args):
    out = [f"*{len(args)}\r\n".encode()]
    for a in args:
        b = a.encode() if isinstance(a, str) else a
        out.append(f"${len(b)}\r\n".encode())
        out.append(b)
        out.append(b"\r\n")
    return b"".join(out)

def read_line(sock, buf):
    while b"\r\n" not in buf:
        data = sock.recv(4096)
        if not data:
            raise EOFError()
        buf += data
    i = buf.index(b"\r\n")
    return buf[:i].decode(), buf[i + 2 :]

def read_n(sock, buf, n):
    while len(buf) < n:
        data = sock.recv(4096)
        if not data:
            raise EOFError()
        buf += data
    return buf[:n], buf[n:]

def decode(sock, buf):
    if not buf:
        buf = sock.recv(4096)
        if not buf:
            raise EOFError()
    t = chr(buf[0]); buf = buf[1:]
    if t in ["+", "-", ":"]:
        line, buf = read_line(sock, buf)
        if t == "+": return line, buf
        if t == "-": raise RespError(line)
        return int(line), buf
    if t == "$":
        line, buf = read_line(sock, buf)
        ln = int(line)
        if ln == -1: return None, buf
        data, buf = read_n(sock, buf, ln + 2)
        return data[:-2].decode(), buf
    if t == "*":
        line, buf = read_line(sock, buf)
        cnt = int(line)
        if cnt == -1: return None, buf
        arr = []
        for _ in range(cnt):
            v, buf = decode(sock, buf)
            arr.append(v)
        return arr, buf
    raise RespError("bad resp")

class MiniMemoryClient:
    def __init__(self, host, port, timeout=5):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def close(self):
        self.sock.close()

    def call(self, *args):
        self.sock.sendall(encode(list(args)))
        v, self.buf = decode(self.sock, self.buf)
        return v

    def auth(self, password):
        return self.call("AUTH", password)
```

#### Go

```go
package minimemory

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"strconv"
)

type Resp interface{}

type Client struct {
	conn net.Conn
	r    *bufio.Reader
}

func Dial(addr string) (*Client, error) {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	return &Client{conn: c, r: bufio.NewReader(c)}, nil
}

func (c *Client) Close() error { return c.conn.Close() }

func encode(args []string) []byte {
	out := []byte(fmt.Sprintf("*%d\r\n", len(args)))
	for _, a := range args {
		out = append(out, []byte(fmt.Sprintf("$%d\r\n", len([]byte(a))))...)
		out = append(out, []byte(a)...)
		out = append(out, []byte("\r\n")...)
	}
	return out
}

func readLine(r *bufio.Reader) (string, error) {
	b, err := r.ReadBytes('\n')
	if err != nil {
		return "", err
	}
	if len(b) < 2 || b[len(b)-2] != '\r' {
		return "", fmt.Errorf("bad line")
	}
	return string(b[:len(b)-2]), nil
}

func decode(r *bufio.Reader) (Resp, error) {
	t, err := r.ReadByte()
	if err != nil {
		return nil, err
	}
	switch t {
	case '+':
		s, err := readLine(r)
		return s, err
	case '-':
		s, err := readLine(r)
		if err != nil {
			return nil, err
		}
		return nil, fmt.Errorf(s)
	case ':':
		s, err := readLine(r)
		if err != nil {
			return nil, err
		}
		n, err := strconv.ParseInt(s, 10, 64)
		return n, err
	case '$':
		s, err := readLine(r)
		if err != nil {
			return nil, err
		}
		n, err := strconv.Atoi(s)
		if err != nil {
			return nil, err
		}
		if n == -1 {
			return nil, nil
		}
		buf := make([]byte, n+2)
		if _, err := io.ReadFull(r, buf); err != nil {
			return nil, err
		}
		return string(buf[:n]), nil
	case '*':
		s, err := readLine(r)
		if err != nil {
			return nil, err
		}
		n, err := strconv.Atoi(s)
		if err != nil {
			return nil, err
		}
		if n == -1 {
			return nil, nil
		}
		arr := make([]Resp, 0, n)
		for i := 0; i < n; i++ {
			v, err := decode(r)
			if err != nil {
				return nil, err
			}
			arr = append(arr, v)
		}
		return arr, nil
	default:
		return nil, fmt.Errorf("bad resp")
	}
}

func (c *Client) Call(args ...string) (Resp, error) {
	if _, err := c.conn.Write(encode(args)); err != nil {
		return nil, err
	}
	return decode(c.r)
}

func (c *Client) Auth(password string) error {
	_, err := c.Call("AUTH", password)
	return err
}
```

#### Rust

```rust
use std::io::{Read, Write};
use std::net::TcpStream;

#[derive(Debug)]
pub enum Resp {
    Str(String),
    Int(i64),
    Bulk(Option<String>),
    Arr(Option<Vec<Resp>>),
}

pub struct Client {
    s: TcpStream,
    buf: Vec<u8>,
}

impl Client {
    pub fn connect(addr: &str) -> std::io::Result<Self> {
        let s = TcpStream::connect(addr)?;
        s.set_nodelay(true).ok();
        Ok(Self { s, buf: Vec::new() })
    }

    pub fn close(self) {}

    pub fn auth(&mut self, password: &str) -> std::io::Result<()> {
        let _ = self.call(&["AUTH", password])?;
        Ok(())
    }

    fn encode(args: &[&str]) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(format!("*{}\r\n", args.len()).as_bytes());
        for a in args {
            let b = a.as_bytes();
            out.extend_from_slice(format!("${}\r\n", b.len()).as_bytes());
            out.extend_from_slice(b);
            out.extend_from_slice(b"\r\n");
        }
        out
    }

    fn read_more(&mut self) -> std::io::Result<()> {
        let mut tmp = [0u8; 4096];
        let n = self.s.read(&mut tmp)?;
        if n == 0 { return Err(std::io::Error::new(std::io::ErrorKind::UnexpectedEof, "eof")); }
        self.buf.extend_from_slice(&tmp[..n]);
        Ok(())
    }

    fn take_line(&mut self, start: usize) -> std::io::Result<Option<(String, usize)>> {
        loop {
            if let Some(p) = self.buf[start..].windows(2).position(|w| w == b"\r\n") {
                let end = start + p;
                let s = String::from_utf8_lossy(&self.buf[start..end]).to_string();
                return Ok(Some((s, end + 2)));
            }
            self.read_more()?;
        }
    }

    fn parse_one(&mut self, mut off: usize) -> std::io::Result<(Resp, usize)> {
        loop {
            if off >= self.buf.len() { self.read_more()?; continue; }
            let t = self.buf[off] as char;
            off += 1;
            match t {
                '+' => {
                    let (line, next) = self.take_line(off)?.unwrap();
                    return Ok((Resp::Str(line), next));
                }
                '-' => {
                    let (line, next) = self.take_line(off)?.unwrap();
                    return Err(std::io::Error::new(std::io::ErrorKind::Other, line));
                }
                ':' => {
                    let (line, next) = self.take_line(off)?.unwrap();
                    let n = line.parse::<i64>().unwrap_or(0);
                    return Ok((Resp::Int(n), next));
                }
                '$' => {
                    let (line, next) = self.take_line(off)?.unwrap();
                    let n = line.parse::<isize>().unwrap_or(-2);
                    if n == -1 { return Ok((Resp::Bulk(None), next)); }
                    let n = n as usize;
                    while self.buf.len() < next + n + 2 { self.read_more()?; }
                    let s = String::from_utf8_lossy(&self.buf[next..next+n]).to_string();
                    return Ok((Resp::Bulk(Some(s)), next + n + 2));
                }
                '*' => {
                    let (line, next) = self.take_line(off)?.unwrap();
                    let n = line.parse::<isize>().unwrap_or(-2);
                    if n == -1 { return Ok((Resp::Arr(None), next)); }
                    let mut cur = next;
                    let mut arr = Vec::with_capacity(n as usize);
                    for _ in 0..(n as usize) {
                        let (v, ncur) = self.parse_one(cur)?;
                        arr.push(v);
                        cur = ncur;
                    }
                    return Ok((Resp::Arr(Some(arr)), cur));
                }
                _ => return Err(std::io::Error::new(std::io::ErrorKind::Other, "bad resp")),
            }
        }
    }

    pub fn call(&mut self, args: &[&str]) -> std::io::Result<Resp> {
        let req = Self::encode(args);
        self.s.write_all(&req)?;
        let (v, used) = self.parse_one(0)?;
        self.buf.drain(0..used);
        Ok(v)
    }
}
```

#### Java

```java
import java.io.*;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public class MiniMemoryClient {
  private final Socket sock;
  private final InputStream in;
  private final OutputStream out;

  public MiniMemoryClient(String host, int port) throws Exception {
    this.sock = new Socket(host, port);
    this.in = sock.getInputStream();
    this.out = sock.getOutputStream();
  }

  public void close() throws Exception { sock.close(); }

  public void auth(String password) throws Exception { call("AUTH", password); }

  private static byte[] encode(String... args) {
    ByteArrayOutputStream b = new ByteArrayOutputStream();
    writeAscii(b, "*" + args.length + "\r\n");
    for (String a : args) {
      byte[] data = a.getBytes(StandardCharsets.UTF_8);
      writeAscii(b, "$" + data.length + "\r\n");
      b.writeBytes(data);
      writeAscii(b, "\r\n");
    }
    return b.toByteArray();
  }

  private static void writeAscii(ByteArrayOutputStream b, String s) {
    b.writeBytes(s.getBytes(StandardCharsets.US_ASCII));
  }

  private String readLine() throws Exception {
    ByteArrayOutputStream b = new ByteArrayOutputStream();
    int prev = -1;
    for (;;) {
      int c = in.read();
      if (c < 0) throw new EOFException();
      b.write(c);
      if (prev == '\r' && c == '\n') break;
      prev = c;
    }
    byte[] arr = b.toByteArray();
    return new String(arr, 0, arr.length - 2, StandardCharsets.UTF_8);
  }

  private Object decode() throws Exception {
    int t = in.read();
    if (t < 0) throw new EOFException();
    if (t == '+') return readLine();
    if (t == '-') throw new RuntimeException(readLine());
    if (t == ':') return Long.parseLong(readLine());
    if (t == '$') {
      int n = Integer.parseInt(readLine());
      if (n == -1) return null;
      byte[] data = in.readNBytes(n);
      in.read(); in.read();
      return new String(data, StandardCharsets.UTF_8);
    }
    if (t == '*') {
      int n = Integer.parseInt(readLine());
      if (n == -1) return null;
      List<Object> arr = new ArrayList<>(n);
      for (int i = 0; i < n; i++) arr.add(decode());
      return arr;
    }
    throw new RuntimeException("bad resp");
  }

  public Object call(String... args) throws Exception {
    out.write(encode(args));
    out.flush();
    return decode();
  }
}
```

#### C/C++（单文件 C++17，POSIX/Windows 二选一可快速适配）

```cpp
#include <string>
#include <vector>
#include <stdexcept>
#include <cctype>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
static int net_init() { WSADATA w; return WSAStartup(MAKEWORD(2,2), &w); }
static void net_deinit() { WSACleanup(); }
static int net_close(SOCKET s) { return closesocket(s); }
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
using SOCKET = int;
static int net_init() { return 0; }
static void net_deinit() {}
static int net_close(SOCKET s) { return close(s); }
#endif

struct Resp {
  enum Type { STR, INT, BULK, ARR, NIL } t = NIL;
  std::string s;
  long long i = 0;
  std::vector<Resp> a;
};

static void send_all(SOCKET fd, const char* p, size_t n) {
  while (n > 0) {
    int k = ::send(fd, p, (int)n, 0);
    if (k <= 0) throw std::runtime_error("send failed");
    p += k;
    n -= (size_t)k;
  }
}

static void recv_some(SOCKET fd, std::string& buf) {
  char tmp[4096];
  int n = ::recv(fd, tmp, sizeof(tmp), 0);
  if (n <= 0) throw std::runtime_error("recv failed");
  buf.append(tmp, tmp + n);
}

static bool take_line(std::string& buf, size_t off, std::string& line, size_t& next) {
  size_t p = buf.find("\r\n", off);
  if (p == std::string::npos) return false;
  line = buf.substr(off, p - off);
  next = p + 2;
  return true;
}

static Resp parse_one(SOCKET fd, std::string& buf, size_t& off) {
  for (;;) {
    if (off >= buf.size()) { recv_some(fd, buf); continue; }
    char t = buf[off++];
    if (t == '+' || t == '-' || t == ':') {
      std::string line; size_t next;
      while (!take_line(buf, off, line, next)) recv_some(fd, buf);
      off = next;
      if (t == '+') { Resp r; r.t = Resp::STR; r.s = line; return r; }
      if (t == '-') throw std::runtime_error(line);
      Resp r; r.t = Resp::INT; r.i = std::stoll(line); return r;
    }
    if (t == '$') {
      std::string line; size_t next;
      while (!take_line(buf, off, line, next)) recv_some(fd, buf);
      off = next;
      int len = std::stoi(line);
      if (len == -1) { Resp r; r.t = Resp::NIL; return r; }
      while (buf.size() < off + (size_t)len + 2) recv_some(fd, buf);
      Resp r; r.t = Resp::BULK; r.s = buf.substr(off, (size_t)len);
      off += (size_t)len + 2;
      return r;
    }
    if (t == '*') {
      std::string line; size_t next;
      while (!take_line(buf, off, line, next)) recv_some(fd, buf);
      off = next;
      int cnt = std::stoi(line);
      if (cnt == -1) { Resp r; r.t = Resp::NIL; return r; }
      Resp r; r.t = Resp::ARR; r.a.reserve((size_t)cnt);
      for (int i = 0; i < cnt; ++i) r.a.push_back(parse_one(fd, buf, off));
      return r;
    }
    throw std::runtime_error("bad resp");
  }
}

class MiniMemoryClient {
public:
  MiniMemoryClient(const std::string& host, int port) {
    if (net_init() != 0) throw std::runtime_error("net init failed");
    struct addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) throw std::runtime_error("dns failed");
    for (auto* p = res; p; p = p->ai_next) {
      fd = (SOCKET)::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd < 0) continue;
      if (::connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) break;
      net_close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) throw std::runtime_error("connect failed");
  }

  ~MiniMemoryClient() { if (fd >= 0) net_close(fd); net_deinit(); }

  Resp call(const std::vector<std::string>& args) {
    std::string req = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) req += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    send_all(fd, req.data(), req.size());
    size_t off = 0;
    Resp r = parse_one(fd, rbuf, off);
    rbuf.erase(0, off);
    return r;
  }

  void auth(const std::string& password) { call({"AUTH", password}); }

private:
  SOCKET fd = -1;
  std::string rbuf;
};
```
