package ai.llama.agent.sdk;

import java.time.Duration;

public final class HttpAgentConfig {
    private final String model;
    private final String systemPrompt;
    private final Duration requestTimeout;

    public HttpAgentConfig(String model) {
        this(model, "", Duration.ofSeconds(300));
    }

    public HttpAgentConfig(String model, String systemPrompt, Duration requestTimeout) {
        this.model = model;
        this.systemPrompt = systemPrompt == null ? "" : systemPrompt;
        this.requestTimeout = requestTimeout == null ? Duration.ofSeconds(300) : requestTimeout;
    }

    public String getModel() {
        return model;
    }

    public String getSystemPrompt() {
        return systemPrompt;
    }

    public Duration getRequestTimeout() {
        return requestTimeout;
    }
}
