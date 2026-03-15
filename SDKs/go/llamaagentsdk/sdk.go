package llamaagentsdk

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

type JSON map[string]any

type ChatMessage struct {
	Role            string `json:"role"`
	Content         any    `json:"content,omitempty"`
	Reasoning       any    `json:"reasoning_content,omitempty"`
	ToolCallID      string `json:"tool_call_id,omitempty"`
	Name            string `json:"name,omitempty"`
	AdditionalProps JSON   `json:"-"`
}

type HttpServerConfig struct {
	BaseURL string
	APIKey  string
}

type HttpAgentConfig struct {
	Model            string
	SystemPrompt     string
	RequestTimeout   time.Duration
	DefaultUserAgent string
}

type HttpAgentSession struct {
	server   HttpServerConfig
	cfg      HttpAgentConfig
	client   *http.Client
	Messages []ChatMessage
}

func NewHttpAgentSession(server HttpServerConfig, cfg HttpAgentConfig) *HttpAgentSession {
	timeout := cfg.RequestTimeout
	if timeout == 0 {
		timeout = 300 * time.Second
	}
	s := &HttpAgentSession{
		server: server,
		cfg:    cfg,
		client: &http.Client{Timeout: timeout},
	}
	if cfg.SystemPrompt != "" {
		s.Messages = append(s.Messages, ChatMessage{Role: "system", Content: cfg.SystemPrompt})
	}
	return s
}

func (s *HttpAgentSession) Clear() {
	var sys *ChatMessage
	if len(s.Messages) > 0 && s.Messages[0].Role == "system" {
		sys = &s.Messages[0]
	}
	s.Messages = nil
	if sys != nil {
		s.Messages = append(s.Messages, *sys)
	}
}

func endpointJoin(prefix, suffix string) string {
	if prefix == "" || prefix == "/" {
		return suffix
	}
	if suffix == "" || suffix == "/" {
		return prefix
	}
	if strings.HasSuffix(prefix, "/") && strings.HasPrefix(suffix, "/") {
		return prefix + suffix[1:]
	}
	if !strings.HasSuffix(prefix, "/") && !strings.HasPrefix(suffix, "/") {
		return prefix + "/" + suffix
	}
	return prefix + suffix
}

func (s *HttpAgentSession) headers() http.Header {
	h := make(http.Header)
	h.Set("Content-Type", "application/json")
	if s.server.APIKey != "" {
		h.Set("Authorization", "Bearer "+s.server.APIKey)
	}
	if s.cfg.DefaultUserAgent != "" {
		h.Set("User-Agent", s.cfg.DefaultUserAgent)
	}
	return h
}

func (s *HttpAgentSession) ChatCompletions(ctx context.Context, userPrompt string, extra JSON) (JSON, error) {
	s.Messages = append(s.Messages, ChatMessage{Role: "user", Content: userPrompt})
	body := JSON{
		"model":    s.cfg.Model,
		"messages": s.Messages,
		"stream":   false,
	}
	for k, v := range extra {
		body[k] = v
	}
	raw, _ := json.Marshal(body)
	url := endpointJoin(strings.TrimRight(s.server.BaseURL, "/"), "/v1/chat/completions")
	req, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(raw))
	if err != nil {
		return nil, err
	}
	req.Header = s.headers()
	res, err := s.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	if res.StatusCode/100 != 2 {
		b, _ := io.ReadAll(res.Body)
		return nil, fmt.Errorf("http %d: %s", res.StatusCode, string(b))
	}
	var out JSON
	if err := json.NewDecoder(res.Body).Decode(&out); err != nil {
		return nil, err
	}
	choices, _ := out["choices"].([]any)
	if len(choices) > 0 {
		c0, _ := choices[0].(map[string]any)
		msg, _ := c0["message"].(map[string]any)
		if msg != nil {
			role, _ := msg["role"].(string)
			content := msg["content"]
			s.Messages = append(s.Messages, ChatMessage{Role: role, Content: content})
		}
	}
	return out, nil
}

type StreamResult struct {
	Usage           JSON
	Content         string
	Reasoning       string
	ToolCallsByIndex map[int]map[string]any
}

func (s *HttpAgentSession) ChatCompletionsStream(ctx context.Context, userPrompt string, onDelta func(string), extra JSON) (StreamResult, error) {
	s.Messages = append(s.Messages, ChatMessage{Role: "user", Content: userPrompt})
	body := JSON{
		"model":    s.cfg.Model,
		"messages": s.Messages,
		"stream":   true,
	}
	for k, v := range extra {
		body[k] = v
	}
	raw, _ := json.Marshal(body)
	url := endpointJoin(strings.TrimRight(s.server.BaseURL, "/"), "/v1/chat/completions")
	req, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(raw))
	if err != nil {
		return StreamResult{}, err
	}
	req.Header = s.headers()
	res, err := s.client.Do(req)
	if err != nil {
		return StreamResult{}, err
	}
	defer res.Body.Close()
	if res.StatusCode/100 != 2 {
		b, _ := io.ReadAll(res.Body)
		return StreamResult{}, fmt.Errorf("http %d: %s", res.StatusCode, string(b))
	}

	var contentParts []string
	var reasoningParts []string
	toolCallsByIndex := map[int]map[string]any{}
	var usage JSON

	sc := bufio.NewScanner(res.Body)
	for sc.Scan() {
		line := strings.TrimRight(sc.Text(), "\r")
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		data := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if data == "" {
			continue
		}
		if data == "[DONE]" {
			break
		}
		var chunk map[string]any
		if err := json.Unmarshal([]byte(data), &chunk); err != nil {
			continue
		}
		if u, ok := chunk["usage"].(map[string]any); ok {
			usage = u
		}
		choices, _ := chunk["choices"].([]any)
		if len(choices) == 0 {
			continue
		}
		c0, _ := choices[0].(map[string]any)
		delta, _ := c0["delta"].(map[string]any)
		if delta == nil {
			continue
		}
		if c, ok := delta["content"].(string); ok && c != "" {
			contentParts = append(contentParts, c)
			if onDelta != nil {
				onDelta(c)
			}
		}
		if r, ok := delta["reasoning_content"].(string); ok && r != "" {
			reasoningParts = append(reasoningParts, r)
		}
		if tcs, ok := delta["tool_calls"].([]any); ok {
			for _, tci := range tcs {
				tc, _ := tci.(map[string]any)
				if tc == nil {
					continue
				}
				idxf, _ := tc["index"].(float64)
				idx := int(idxf)
				acc := toolCallsByIndex[idx]
				if acc == nil {
					acc = map[string]any{
						"id": nil,
						"function": map[string]any{
							"name":      nil,
							"arguments": "",
						},
					}
					toolCallsByIndex[idx] = acc
				}
				if id, ok := tc["id"].(string); ok {
					acc["id"] = id
				}
				fn, _ := tc["function"].(map[string]any)
				if fn != nil {
					fna, _ := acc["function"].(map[string]any)
					if name, ok := fn["name"].(string); ok {
						fna["name"] = name
					}
					if args, ok := fn["arguments"].(string); ok {
						fna["arguments"] = fna["arguments"].(string) + args
					}
				}
			}
		}
	}
	content := strings.Join(contentParts, "")
	reasoning := strings.Join(reasoningParts, "")
	s.Messages = append(s.Messages, ChatMessage{Role: "assistant", Content: content, Reasoning: reasoning})

	return StreamResult{
		Usage:           usage,
		Content:         content,
		Reasoning:       reasoning,
		ToolCallsByIndex: toolCallsByIndex,
	}, sc.Err()
}

