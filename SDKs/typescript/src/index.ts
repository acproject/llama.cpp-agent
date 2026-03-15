export type Json = Record<string, unknown>;

export type ChatMessage = {
  role: "system" | "user" | "assistant" | "tool";
  content?: string | null;
  reasoning_content?: string | null;
  tool_call_id?: string;
  name?: string;
  tool_calls?: ToolCall[];
};

export type ToolCallDelta = {
  index: number;
  id?: string;
  function?: {
    name?: string;
    arguments?: string;
  };
};

export type ToolCall = {
  id: string;
  type: "function";
  function: {
    name: string;
    arguments: string;
  };
};

export type ChatCompletionChunk = {
  choices?: Array<{
    delta?: {
      content?: string | null;
      reasoning_content?: string | null;
      tool_calls?: ToolCallDelta[];
    };
  }>;
  usage?: Json;
};

export type HttpServerConfig = {
  baseUrl: string;
  apiKey?: string;
};

export type HttpAgentConfig = {
  model: string;
  workingDir?: string;
  systemPrompt?: string;
  requestTimeoutMs?: number;
};

function endpointJoin(prefix: string, suffix: string): string {
  if (!prefix || prefix === "/") return suffix;
  if (!suffix || suffix === "/") return prefix;
  if (prefix.endsWith("/") && suffix.startsWith("/")) return prefix + suffix.slice(1);
  if (!prefix.endsWith("/") && !suffix.startsWith("/")) return prefix + "/" + suffix;
  return prefix + suffix;
}

async function* sseDataEvents(body: ReadableStream<Uint8Array>): AsyncGenerator<string> {
  const reader = body.getReader();
  const decoder = new TextDecoder("utf-8");
  let buf = "";
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += decoder.decode(value, { stream: true });
    while (true) {
      const nl = buf.indexOf("\n");
      if (nl < 0) break;
      const line = buf.slice(0, nl).replace(/\r$/, "");
      buf = buf.slice(nl + 1);
      if (!line.startsWith("data:")) continue;
      const data = line.slice("data:".length).trim();
      if (!data) continue;
      if (data === "[DONE]") return;
      yield data;
    }
  }
}

export class HttpAgentSession {
  private server: HttpServerConfig;
  private cfg: HttpAgentConfig;
  private _messages: ChatMessage[] = [];

  constructor(server: HttpServerConfig, cfg: HttpAgentConfig) {
    this.server = server;
    this.cfg = cfg;
    if (cfg.systemPrompt && cfg.systemPrompt.length > 0) {
      this._messages.push({ role: "system", content: cfg.systemPrompt });
    }
  }

  get messages(): ChatMessage[] {
    return this._messages;
  }

  clear(): void {
    const sys = this._messages.length > 0 && this._messages[0].role === "system" ? this._messages[0] : undefined;
    this._messages = [];
    if (sys) this._messages.push(sys);
  }

  private headers(): Record<string, string> {
    const h: Record<string, string> = { "Content-Type": "application/json" };
    if (this.server.apiKey && this.server.apiKey.length > 0) h["Authorization"] = `Bearer ${this.server.apiKey}`;
    return h;
  }

  async chatCompletions(userPrompt: string, extra?: Json): Promise<Json> {
    this._messages.push({ role: "user", content: userPrompt });
    const body: Json = { model: this.cfg.model, messages: this._messages, stream: false, ...(extra ?? {}) };
    const url = endpointJoin(this.server.baseUrl.replace(/\/$/, ""), "/v1/chat/completions");
    const res = await fetch(url, {
      method: "POST",
      headers: this.headers(),
      body: JSON.stringify(body),
      signal: this.cfg.requestTimeoutMs ? AbortSignal.timeout(this.cfg.requestTimeoutMs) : undefined
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const out = (await res.json()) as Json;
    const choices = (out["choices"] as any[]) ?? [];
    const msg = choices[0]?.message as ChatMessage | undefined;
    if (msg && msg.role) this._messages.push(msg);
    return out;
  }

  async chatCompletionsStream(
    userPrompt: string,
    onDelta?: (textDelta: string) => void,
    extra?: Json
  ): Promise<{ usage?: Json; content: string; reasoning: string; toolCallsByIndex: Map<number, any> }> {
    this._messages.push({ role: "user", content: userPrompt });
    const body: Json = { model: this.cfg.model, messages: this._messages, stream: true, ...(extra ?? {}) };
    const url = endpointJoin(this.server.baseUrl.replace(/\/$/, ""), "/v1/chat/completions");
    const res = await fetch(url, {
      method: "POST",
      headers: this.headers(),
      body: JSON.stringify(body),
      signal: this.cfg.requestTimeoutMs ? AbortSignal.timeout(this.cfg.requestTimeoutMs) : undefined
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    if (!res.body) throw new Error("Missing response body");

    const contentParts: string[] = [];
    const reasoningParts: string[] = [];
    const toolCallsByIndex = new Map<number, { id?: string; name?: string; arguments: string }>();
    let usage: Json | undefined;

    for await (const data of sseDataEvents(res.body)) {
      const chunk = JSON.parse(data) as ChatCompletionChunk;
      if (chunk.usage && typeof chunk.usage === "object") usage = chunk.usage as Json;
      const delta = chunk.choices?.[0]?.delta;
      if (!delta) continue;
      if (typeof delta.content === "string" && delta.content.length > 0) {
        contentParts.push(delta.content);
        if (onDelta) onDelta(delta.content);
      }
      if (typeof delta.reasoning_content === "string" && delta.reasoning_content.length > 0) {
        reasoningParts.push(delta.reasoning_content);
      }
      for (const tc of delta.tool_calls ?? []) {
        const idx = Number(tc.index);
        if (!Number.isFinite(idx)) continue;
        const acc = toolCallsByIndex.get(idx) ?? { arguments: "" };
        if (typeof tc.id === "string") acc.id = tc.id;
        if (typeof tc.function?.name === "string") acc.name = tc.function.name;
        if (typeof tc.function?.arguments === "string") acc.arguments += tc.function.arguments;
        toolCallsByIndex.set(idx, acc);
      }
    }

    const content = contentParts.join("");
    const reasoning = reasoningParts.join("");
    const assistantMsg: ChatMessage = { role: "assistant", content };
    if (reasoning.length > 0) assistantMsg.reasoning_content = reasoning;
    if (toolCallsByIndex.size > 0) {
      assistantMsg.tool_calls = Array.from(toolCallsByIndex.entries())
        .sort((a, b) => a[0] - b[0])
        .map(([idx, tc]): ToolCall => ({
          id: tc.id ?? `call_${idx}`,
          type: "function",
          function: { name: tc.name ?? "", arguments: tc.arguments ?? "" }
        }));
    }
    this._messages.push(assistantMsg);
    return { usage, content, reasoning, toolCallsByIndex };
  }
}
