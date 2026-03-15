package ai.llama.agent.sdk;

public final class HttpServerConfig {
    private final String baseUrl;
    private final String apiKey;

    public HttpServerConfig(String baseUrl) {
        this(baseUrl, "");
    }

    public HttpServerConfig(String baseUrl, String apiKey) {
        this.baseUrl = baseUrl;
        this.apiKey = apiKey == null ? "" : apiKey;
    }

    public String getBaseUrl() {
        return baseUrl;
    }

    public String getApiKey() {
        return apiKey;
    }
}
