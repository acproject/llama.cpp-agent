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
export declare class HttpAgentSession {
    private server;
    private cfg;
    private _messages;
    constructor(server: HttpServerConfig, cfg: HttpAgentConfig);
    get messages(): ChatMessage[];
    clear(): void;
    private headers;
    chatCompletions(userPrompt: string, extra?: Json): Promise<Json>;
    chatCompletionsStream(userPrompt: string, onDelta?: (textDelta: string) => void, extra?: Json): Promise<{
        usage?: Json;
        content: string;
        reasoning: string;
        toolCallsByIndex: Map<number, any>;
    }>;
}
