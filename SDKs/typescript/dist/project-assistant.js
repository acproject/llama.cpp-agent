import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { HttpAgentSession } from "./index.js";
import { MiniMemoryClient, respToJson } from "./minimemory.js";
function getArg(argv, name, def = "") {
    const i = argv.indexOf(name);
    if (i >= 0 && i + 1 < argv.length)
        return argv[i + 1] ?? def;
    return def;
}
function hasFlag(argv, name) {
    return argv.includes(name);
}
async function readStdinAll() {
    const chunks = [];
    for await (const c of process.stdin)
        chunks.push(c);
    return Buffer.concat(chunks).toString("utf8").trim();
}
function toolSchema(name, description, parameters) {
    return { type: "function", function: { name, description, parameters } };
}
function toolResultMessage(callId, name, content) {
    return { role: "tool", tool_call_id: callId, name, content };
}
function safeJsonParse(s) {
    try {
        return JSON.parse(s);
    }
    catch {
        return null;
    }
}
function endpointJoin(prefix, suffix) {
    if (!prefix || prefix === "/")
        return suffix;
    if (!suffix || suffix === "/")
        return prefix;
    if (prefix.endsWith("/") && suffix.startsWith("/"))
        return prefix + suffix.slice(1);
    if (!prefix.endsWith("/") && !suffix.startsWith("/"))
        return prefix + "/" + suffix;
    return prefix + suffix;
}
async function webSearch(query, maxResults) {
    const url = `https://duckduckgo.com/html/?q=${encodeURIComponent(query)}`;
    const res = await fetch(url, { method: "GET" });
    if (!res.ok)
        return { content: `web_search failed: HTTP ${res.status}` };
    const html = await res.text();
    const out = [];
    const re = /<a[^>]*class="result__a"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g;
    let m;
    while ((m = re.exec(html)) && out.length < maxResults) {
        const href = m[1] ?? "";
        const titleRaw = m[2] ?? "";
        const title = titleRaw.replace(/<[^>]+>/g, "").replace(/\s+/g, " ").trim();
        const u = href.replace(/&amp;/g, "&");
        if (title && u)
            out.push({ title, url: u });
    }
    return { content: JSON.stringify({ query, results: out }, null, 2) };
}
async function webFetch(url, maxChars) {
    const res = await fetch(url, { method: "GET" });
    if (!res.ok)
        return { content: `web_fetch failed: HTTP ${res.status}` };
    const ct = res.headers.get("content-type") ?? "";
    const raw = await res.text();
    let text = raw;
    if (ct.includes("text/html")) {
        text = raw
            .replace(/<script[\s\S]*?<\/script>/gi, "")
            .replace(/<style[\s\S]*?<\/style>/gi, "")
            .replace(/<[^>]+>/g, " ")
            .replace(/\s+/g, " ")
            .trim();
    }
    if (text.length > maxChars)
        text = text.slice(0, maxChars);
    return { content: JSON.stringify({ url, content_type: ct, text }, null, 2) };
}
async function loadSkillsSection(workingDir) {
    const dirs = [];
    dirs.push(path.join(workingDir, ".llama-agent", "skills"));
    const home = os.homedir();
    if (home)
        dirs.push(path.join(home, ".llama-agent", "skills"));
    const files = [];
    for (const d of dirs) {
        try {
            const ents = await fs.readdir(d, { withFileTypes: true });
            for (const e of ents) {
                if (!e.isFile())
                    continue;
                files.push(path.join(d, e.name));
            }
        }
        catch {
        }
    }
    files.sort();
    const chunks = [];
    for (const f of files) {
        try {
            const c = await fs.readFile(f, "utf8");
            if (c.trim().length > 0)
                chunks.push(c.trim());
        }
        catch {
        }
    }
    if (chunks.length === 0)
        return "";
    return "\n\n# Skills\n\n" + chunks.join("\n\n");
}
async function llamaEmbeddings(baseUrl, model, input) {
    const url = endpointJoin(baseUrl.replace(/\/$/, ""), "/v1/embeddings");
    const body = { model, input, encoding_format: "float" };
    let res;
    try {
        res = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    }
    catch (e) {
        throw new Error(`embeddings ${formatFetchError(e, url)}`);
    }
    if (!res.ok) {
        throw new Error(`embeddings http ${res.status}`);
    }
    const out = (await res.json());
    const emb = out?.data?.[0]?.embedding;
    if (!Array.isArray(emb)) {
        throw new Error("invalid embeddings response");
    }
    const vec = emb.map((x) => Number(x)).filter((x) => Number.isFinite(x));
    if (vec.length === 0)
        throw new Error("empty embedding vector");
    return vec;
}
function formatFetchError(e, url) {
    const parts = [];
    const cause = e?.cause;
    if (cause && typeof cause === "object") {
        const code = cause.code ?? cause.errno;
        const msg = cause.message;
        if (code)
            parts.push(String(code));
        if (msg)
            parts.push(String(msg));
    }
    const extra = parts.length > 0 ? `: ${parts.join(" ")}` : "";
    return `fetch failed (${url})${extra}`;
}
async function preflight(baseUrl) {
    const healthUrl = endpointJoin(baseUrl.replace(/\/$/, ""), "/health");
    try {
        const res = await fetch(healthUrl, { method: "GET" });
        if (!res.ok) {
            const t = await res.text().catch(() => "");
            throw new Error(`HTTP ${res.status}${t ? `: ${t.slice(0, 4000)}` : ""}`);
        }
    }
    catch (e) {
        throw new Error(`server unreachable: ${formatFetchError(e, healthUrl)}`);
    }
}
async function ragSearchMiniMemory(mm, embedder, query, topK, metric, dimHint, opts) {
    let vec = [];
    const embInput = `query: ${query}`;
    try {
        const emb = await mm.command(["EMBED", "QUERY", embInput]);
        if (emb.type !== "error") {
            const vecAny = respToJson(emb);
            vec = Array.isArray(vecAny) ? vecAny.map((x) => Number(x)) : [];
        }
    }
    catch {
    }
    if (vec.length === 0) {
        try {
            vec = await llamaEmbeddings(embedder.baseUrl, embedder.model, embInput);
        }
        catch (e) {
            return { content: `rag_search failed: embeddings unavailable (${String(e?.message ?? e)})` };
        }
    }
    const dim = Number.isFinite(dimHint ?? NaN) ? dimHint : vec.length;
    if (vec.length === 0 || dim <= 0 || vec.length < dim) {
        return { content: JSON.stringify({ error: "invalid embedding vector", vec_sample: vec.slice(0, 8) }, null, 2) };
    }
    const args = ["EVIDENCE.SEARCHF", String(topK), metric, String(dim), ...vec.slice(0, dim).map((x) => String(x))];
    const tags = opts?.tags ?? [];
    for (const t of tags) {
        if (typeof t === "string" && t.length > 0)
            args.push("TAG", t);
    }
    const meta = opts?.meta ?? {};
    for (const [k, v] of Object.entries(meta)) {
        if (k && v != null)
            args.push("META", String(k), String(v));
    }
    if (opts?.keyPrefix)
        args.push("KEYPREFIX", String(opts.keyPrefix));
    if (opts?.graphChain && Array.isArray(opts.graphChain) && opts.graphChain.length > 0) {
        args.push("GRAPHCHAINLEN", String(opts.graphChain.length), ...opts.graphChain.map((x) => String(x)));
    }
    else {
        if (opts?.graphFrom)
            args.push("GRAPHFROM", String(opts.graphFrom));
        if (opts?.graphRel)
            args.push("GRAPHREL", String(opts.graphRel));
        if (opts?.graphDepth && Number.isFinite(opts.graphDepth))
            args.push("GRAPHDEPTH", String(opts.graphDepth));
    }
    const r = await mm.command(args);
    if (r.type === "error")
        return { content: `rag_search failed: ${r.value}` };
    return {
        content: JSON.stringify({ query, top_k: topK, metric, dim, filters: opts ?? {}, result: respToJson(r) }, null, 2)
    };
}
async function runAgentLoop(opts) {
    let lastText = "";
    for (let i = 0; i < opts.maxIterations; i++) {
        let assistantDelta = "";
        const extra = { tools: opts.tools, tool_choice: "auto" };
        await opts.session.chatCompletionsStream(i === 0 ? opts.prompt : "", (d) => {
            assistantDelta += d;
            if (opts.streamToStdout)
                process.stdout.write(d);
        }, extra);
        lastText = assistantDelta;
        const msg = opts.session.messages[opts.session.messages.length - 1];
        const toolCalls = msg?.role === "assistant" ? (msg.tool_calls ?? []) : [];
        if (toolCalls.length === 0)
            return lastText;
        for (const tc of toolCalls) {
            const name = tc.function.name;
            const args = safeJsonParse(tc.function.arguments) ?? {};
            const r = await opts.toolRunner(name, args);
            opts.session.messages.push(toolResultMessage(tc.id, name, r.content));
        }
    }
    return lastText;
}
async function main() {
    const argv = process.argv.slice(2);
    const url = getArg(argv, "--url");
    const model = getArg(argv, "--model");
    const promptArg = getArg(argv, "--prompt");
    const workingDir = getArg(argv, "--working-dir", ".");
    const mmHost = getArg(argv, "--minimemory-host", "127.0.0.1");
    const mmPort = Number(getArg(argv, "--minimemory-port", "6399"));
    const mmPass = getArg(argv, "--minimemory-pass", process.env.MINIMEMORY_PASS ?? "");
    const embedModel = getArg(argv, "--embed-model", "multilingual-e5-large-instruct");
    const webModel = getArg(argv, "--web-model", model);
    const writerModel = getArg(argv, "--writer-model", model);
    const noStream = hasFlag(argv, "--no-stream");
    if (!url || !model) {
        process.stderr.write("Usage:\n  node dist/project-assistant.js --url http://127.0.0.1:8080 --model MODEL [--prompt \"...\"] [--working-dir .] [--minimemory-host 127.0.0.1 --minimemory-port 6399] [--minimemory-pass ...] [--embed-model multilingual-e5-large-instruct]\n");
        process.exit(2);
    }
    const prompt = promptArg || (await readStdinAll());
    if (!prompt)
        process.exit(2);
    await preflight(url);
    const skills = await loadSkillsSection(workingDir);
    const baseSystemPrompt = "You are a project assistant. Use RAG and web sources when needed, and always cite sources.";
    const systemPrompt = baseSystemPrompt + skills;
    const session = new HttpAgentSession({ baseUrl: url }, { model, workingDir, systemPrompt, requestTimeoutMs: 300000 });
    const mm = new MiniMemoryClient({ host: mmHost, port: mmPort, password: mmPass || undefined });
    const embedder = { baseUrl: url, model: embedModel };
    const ragTool = toolSchema("rag_search", "Query MiniMemory RAG and return structured evidence.", {
        type: "object",
        properties: {
            query: { type: "string" },
            top_k: { type: "integer", minimum: 1, maximum: 50 },
            metric: { type: "string", enum: ["cosine"] },
            dim: { type: "integer", minimum: 1, maximum: 8192 },
            tags: { type: "array", items: { type: "string" } },
            meta: { type: "object", additionalProperties: { type: "string" } },
            key_prefix: { type: "string" },
            graph_from: { type: "string" },
            graph_rel: { type: "string" },
            graph_depth: { type: "integer", minimum: 0, maximum: 64 },
            graph_chain: { type: "array", items: { type: "string" } }
        },
        required: ["query"]
    });
    const taskTool = toolSchema("task", "Run a specialized subagent with restricted tools.", {
        type: "object",
        properties: {
            subagent_type: { type: "string", enum: ["web", "writer"] },
            prompt: { type: "string" }
        },
        required: ["subagent_type", "prompt"]
    });
    const webSearchTool = toolSchema("web_search", "Search the web and return a short list of results.", {
        type: "object",
        properties: { query: { type: "string" }, max_results: { type: "integer", minimum: 1, maximum: 10 } },
        required: ["query"]
    });
    const webFetchTool = toolSchema("web_fetch", "Fetch a webpage and return extracted text.", {
        type: "object",
        properties: { url: { type: "string" }, max_chars: { type: "integer", minimum: 500, maximum: 200000 } },
        required: ["url"]
    });
    const mainTools = [ragTool, taskTool];
    async function runWebSubagent(p) {
        const s = new HttpAgentSession({ baseUrl: url }, { model: webModel, workingDir, systemPrompt: "You are a web research assistant." });
        const tools = [webSearchTool, webFetchTool];
        const toolRunner = async (name, args) => {
            if (name === "web_search")
                return webSearch(String(args.query ?? ""), Number(args.max_results ?? 5));
            if (name === "web_fetch")
                return webFetch(String(args.url ?? ""), Number(args.max_chars ?? 12000));
            return { content: `unknown tool: ${name}` };
        };
        const out = await runAgentLoop({ session: s, prompt: p, tools, toolRunner, maxIterations: 12, streamToStdout: false });
        return out;
    }
    async function runWriterSubagent(p) {
        const writerPrompt = baseSystemPrompt + skills;
        const s = new HttpAgentSession({ baseUrl: url }, { model: writerModel, workingDir, systemPrompt: writerPrompt });
        const tools = [ragTool];
        const toolRunner = async (name, args) => {
            if (name === "rag_search") {
                return ragSearchMiniMemory(mm, embedder, String(args.query ?? ""), Number(args.top_k ?? 5), String(args.metric ?? "cosine"), args.dim ? Number(args.dim) : undefined, {
                    tags: Array.isArray(args.tags) ? args.tags.map((x) => String(x)) : undefined,
                    meta: args.meta && typeof args.meta === "object" ? args.meta : undefined,
                    keyPrefix: args.key_prefix ? String(args.key_prefix) : undefined,
                    graphFrom: args.graph_from ? String(args.graph_from) : undefined,
                    graphRel: args.graph_rel ? String(args.graph_rel) : undefined,
                    graphDepth: args.graph_depth != null ? Number(args.graph_depth) : undefined,
                    graphChain: Array.isArray(args.graph_chain) ? args.graph_chain.map((x) => String(x)) : undefined
                });
            }
            return { content: `unknown tool: ${name}` };
        };
        const out = await runAgentLoop({ session: s, prompt: p, tools, toolRunner, maxIterations: 16, streamToStdout: false });
        return out;
    }
    const toolRunner = async (name, args) => {
        if (name === "rag_search") {
            return ragSearchMiniMemory(mm, embedder, String(args.query ?? ""), Number(args.top_k ?? 5), String(args.metric ?? "cosine"), args.dim ? Number(args.dim) : undefined, {
                tags: Array.isArray(args.tags) ? args.tags.map((x) => String(x)) : undefined,
                meta: args.meta && typeof args.meta === "object" ? args.meta : undefined,
                keyPrefix: args.key_prefix ? String(args.key_prefix) : undefined,
                graphFrom: args.graph_from ? String(args.graph_from) : undefined,
                graphRel: args.graph_rel ? String(args.graph_rel) : undefined,
                graphDepth: args.graph_depth != null ? Number(args.graph_depth) : undefined,
                graphChain: Array.isArray(args.graph_chain) ? args.graph_chain.map((x) => String(x)) : undefined
            });
        }
        if (name === "task") {
            const t = String(args.subagent_type ?? "");
            const p = String(args.prompt ?? "");
            if (t === "web")
                return { content: await runWebSubagent(p) };
            if (t === "writer")
                return { content: await runWriterSubagent(p) };
            return { content: `unknown subagent_type: ${t}` };
        }
        return { content: `unknown tool: ${name}` };
    };
    const out = await runAgentLoop({
        session,
        prompt,
        tools: mainTools,
        toolRunner,
        maxIterations: 20,
        streamToStdout: !noStream
    });
    if (noStream)
        process.stdout.write(out + "\n");
    else
        process.stdout.write("\n");
}
if (process.argv[1] && process.argv[1].includes("project-assistant")) {
    main().catch((e) => {
        process.stderr.write(String(e?.message ?? e) + "\n");
        process.exit(1);
    });
}
