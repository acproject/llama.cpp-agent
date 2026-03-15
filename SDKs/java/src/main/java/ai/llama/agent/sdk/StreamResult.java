package ai.llama.agent.sdk;

import com.fasterxml.jackson.databind.JsonNode;
import java.util.Map;

public final class StreamResult {
    private final JsonNode usage;
    private final String content;
    private final String reasoning;
    private final Map<Integer, ToolCallAcc> toolCallsByIndex;

    public StreamResult(JsonNode usage, String content, String reasoning, Map<Integer, ToolCallAcc> toolCallsByIndex) {
        this.usage = usage;
        this.content = content;
        this.reasoning = reasoning;
        this.toolCallsByIndex = toolCallsByIndex;
    }

    public JsonNode getUsage() {
        return usage;
    }

    public String getContent() {
        return content;
    }

    public String getReasoning() {
        return reasoning;
    }

    public Map<Integer, ToolCallAcc> getToolCallsByIndex() {
        return toolCallsByIndex;
    }

    public static final class ToolCallAcc {
        private String id;
        private String name;
        private String arguments;

        public ToolCallAcc() {
            this.arguments = "";
        }

        public String getId() {
            return id;
        }

        public void setId(String id) {
            this.id = id;
        }

        public String getName() {
            return name;
        }

        public void setName(String name) {
            this.name = name;
        }

        public String getArguments() {
            return arguments;
        }

        public void appendArguments(String delta) {
            if (delta != null && !delta.isEmpty()) {
                this.arguments = this.arguments + delta;
            }
        }
    }
}
