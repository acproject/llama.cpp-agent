package ai.llama.agent.sdk;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

public final class HttpAgentSession {
    private final HttpServerConfig server;
    private final HttpAgentConfig cfg;
    private final HttpClient client;
    private final ObjectMapper mapper = new ObjectMapper();
    private final List<ObjectNode> messages = new ArrayList<>();

    public HttpAgentSession(HttpServerConfig server, HttpAgentConfig cfg) {
        this.server = server;
        this.cfg = cfg;
        this.client = HttpClient.newBuilder()
                .connectTimeout(Duration.ofSeconds(30))
                .build();
        if (cfg.getSystemPrompt() != null && !cfg.getSystemPrompt().isEmpty()) {
            ObjectNode sys = mapper.createObjectNode();
            sys.put("role", "system");
            sys.put("content", cfg.getSystemPrompt());
            messages.add(sys);
        }
    }

    public List<ObjectNode> getMessages() {
        return messages;
    }

    public void clear() {
        ObjectNode sys = null;
        if (!messages.isEmpty() && "system".equals(messages.get(0).path("role").asText())) {
            sys = messages.get(0);
        }
        messages.clear();
        if (sys != null) {
            messages.add(sys);
        }
    }

    private static String endpointJoin(String prefix, String suffix) {
        if (prefix == null || prefix.isEmpty() || "/".equals(prefix)) {
            return suffix;
        }
        if (suffix == null || suffix.isEmpty() || "/".equals(suffix)) {
            return prefix;
        }
        if (prefix.endsWith("/") && suffix.startsWith("/")) {
            return prefix + suffix.substring(1);
        }
        if (!prefix.endsWith("/") && !suffix.startsWith("/")) {
            return prefix + "/" + suffix;
        }
        return prefix + suffix;
    }

    private HttpRequest.Builder baseRequest(String url) {
        HttpRequest.Builder b = HttpRequest.newBuilder().uri(URI.create(url));
        b.header("Content-Type", "application/json");
        if (server.getApiKey() != null && !server.getApiKey().isEmpty()) {
            b.header("Authorization", "Bearer " + server.getApiKey());
        }
        return b;
    }

    public JsonNode chatCompletions(String userPrompt, ObjectNode extra) throws Exception {
        ObjectNode user = mapper.createObjectNode();
        user.put("role", "user");
        user.put("content", userPrompt);
        messages.add(user);

        ObjectNode body = mapper.createObjectNode();
        body.put("model", cfg.getModel());
        body.set("messages", mapper.valueToTree(messages));
        body.put("stream", false);
        if (extra != null) {
            body.setAll(extra);
        }

        String url = endpointJoin(trimRightSlash(server.getBaseUrl()), "/v1/chat/completions");
        HttpRequest req = baseRequest(url)
                .timeout(cfg.getRequestTimeout())
                .POST(HttpRequest.BodyPublishers.ofString(body.toString(), StandardCharsets.UTF_8))
                .build();
        HttpResponse<String> res = client.send(req, HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        if (res.statusCode() / 100 != 2) {
            throw new RuntimeException("HTTP " + res.statusCode() + ": " + res.body());
        }
        JsonNode out = mapper.readTree(res.body());
        JsonNode msg = out.path("choices").path(0).path("message");
        if (!msg.isMissingNode() && msg.isObject()) {
            messages.add((ObjectNode) msg);
        }
        return out;
    }

    public StreamResult chatCompletionsStream(String userPrompt, Consumer<String> onDelta, ObjectNode extra)
            throws Exception {
        ObjectNode user = mapper.createObjectNode();
        user.put("role", "user");
        user.put("content", userPrompt);
        messages.add(user);

        ObjectNode body = mapper.createObjectNode();
        body.put("model", cfg.getModel());
        body.set("messages", mapper.valueToTree(messages));
        body.put("stream", true);
        if (extra != null) {
            body.setAll(extra);
        }

        String url = endpointJoin(trimRightSlash(server.getBaseUrl()), "/v1/chat/completions");
        HttpRequest req = baseRequest(url)
                .timeout(cfg.getRequestTimeout())
                .POST(HttpRequest.BodyPublishers.ofString(body.toString(), StandardCharsets.UTF_8))
                .build();
        HttpResponse<java.io.InputStream> res =
                client.send(req, HttpResponse.BodyHandlers.ofInputStream());
        if (res.statusCode() / 100 != 2) {
            String err = new String(res.body().readAllBytes(), StandardCharsets.UTF_8);
            throw new RuntimeException("HTTP " + res.statusCode() + ": " + err);
        }

        StringBuilder content = new StringBuilder();
        StringBuilder reasoning = new StringBuilder();
        Map<Integer, StreamResult.ToolCallAcc> toolCallsByIndex = new HashMap<>();
        JsonNode usage = null;

        try (BufferedReader r = new BufferedReader(new InputStreamReader(res.body(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.replaceAll("\\r$", "");
                if (!line.startsWith("data:")) {
                    continue;
                }
                String data = line.substring("data:".length()).trim();
                if (data.isEmpty()) {
                    continue;
                }
                if ("[DONE]".equals(data)) {
                    break;
                }
                JsonNode chunk;
                try {
                    chunk = mapper.readTree(data);
                } catch (Exception ignored) {
                    continue;
                }
                if (chunk.has("usage") && chunk.get("usage").isObject()) {
                    usage = chunk.get("usage");
                }
                JsonNode delta = chunk.path("choices").path(0).path("delta");
                if (delta.isMissingNode() || !delta.isObject()) {
                    continue;
                }
                JsonNode c = delta.get("content");
                if (c != null && c.isTextual()) {
                    String s = c.asText();
                    if (!s.isEmpty()) {
                        content.append(s);
                        if (onDelta != null) {
                            onDelta.accept(s);
                        }
                    }
                }
                JsonNode rr = delta.get("reasoning_content");
                if (rr != null && rr.isTextual()) {
                    String s = rr.asText();
                    if (!s.isEmpty()) {
                        reasoning.append(s);
                    }
                }
                JsonNode tcs = delta.get("tool_calls");
                if (tcs != null && tcs.isArray()) {
                    for (JsonNode tc : tcs) {
                        int idx = tc.path("index").asInt(-1);
                        if (idx < 0) {
                            continue;
                        }
                        StreamResult.ToolCallAcc acc = toolCallsByIndex.computeIfAbsent(idx, k -> new StreamResult.ToolCallAcc());
                        JsonNode id = tc.get("id");
                        if (id != null && id.isTextual()) {
                            acc.setId(id.asText());
                        }
                        JsonNode fn = tc.get("function");
                        if (fn != null && fn.isObject()) {
                            JsonNode name = fn.get("name");
                            if (name != null && name.isTextual()) {
                                acc.setName(name.asText());
                            }
                            JsonNode args = fn.get("arguments");
                            if (args != null && args.isTextual()) {
                                acc.appendArguments(args.asText());
                            }
                        }
                    }
                }
            }
        }

        ObjectNode assistant = mapper.createObjectNode();
        assistant.put("role", "assistant");
        assistant.put("content", content.toString());
        if (reasoning.length() > 0) {
            assistant.put("reasoning_content", reasoning.toString());
        }
        if (!toolCallsByIndex.isEmpty()) {
            ArrayNode outCalls = mapper.createArrayNode();
            toolCallsByIndex.entrySet().stream()
                    .sorted(Comparator.comparingInt(Map.Entry::getKey))
                    .forEach(e -> {
                        int idx = e.getKey();
                        StreamResult.ToolCallAcc acc = e.getValue();
                        ObjectNode tc = mapper.createObjectNode();
                        tc.put("id", acc.getId() == null ? ("call_" + idx) : acc.getId());
                        tc.put("type", "function");
                        ObjectNode fn = mapper.createObjectNode();
                        fn.put("name", acc.getName() == null ? "" : acc.getName());
                        fn.put("arguments", acc.getArguments() == null ? "" : acc.getArguments());
                        tc.set("function", fn);
                        outCalls.add(tc);
                    });
            assistant.set("tool_calls", outCalls);
        }
        messages.add(assistant);

        return new StreamResult(usage, content.toString(), reasoning.toString(), toolCallsByIndex);
    }

    private static String trimRightSlash(String s) {
        if (s == null) {
            return "";
        }
        if (s.endsWith("/")) {
            return s.substring(0, s.length() - 1);
        }
        return s;
    }
}
