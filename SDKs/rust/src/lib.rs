use reqwest::blocking::Client;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::io::{BufRead, BufReader};
use std::time::Duration;

#[derive(Clone, Debug)]
pub struct HttpServerConfig {
    pub base_url: String,
    pub api_key: Option<String>,
}

#[derive(Clone, Debug)]
pub struct HttpAgentConfig {
    pub model: String,
    pub system_prompt: Option<String>,
    pub request_timeout: Duration,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ChatMessage {
    pub role: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub content: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reasoning_content: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tool_call_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
}

#[derive(Clone, Debug)]
pub struct StreamResult {
    pub usage: Option<Value>,
    pub content: String,
    pub reasoning: String,
    pub tool_calls_by_index: BTreeMap<i64, Value>,
}

fn endpoint_join(prefix: &str, suffix: &str) -> String {
    if prefix.is_empty() || prefix == "/" {
        return suffix.to_string();
    }
    if suffix.is_empty() || suffix == "/" {
        return prefix.to_string();
    }
    if prefix.ends_with('/') && suffix.starts_with('/') {
        return format!("{}{}", prefix, &suffix[1..]);
    }
    if !prefix.ends_with('/') && !suffix.starts_with('/') {
        return format!("{}/{}", prefix, suffix);
    }
    format!("{}{}", prefix, suffix)
}

pub struct HttpAgentSession {
    server: HttpServerConfig,
    cfg: HttpAgentConfig,
    client: Client,
    pub messages: Vec<ChatMessage>,
}

impl HttpAgentSession {
    pub fn new(server: HttpServerConfig, cfg: HttpAgentConfig) -> Result<Self, reqwest::Error> {
        let client = Client::builder().timeout(cfg.request_timeout).build()?;
        let mut messages = Vec::new();
        if let Some(sp) = cfg.system_prompt.clone() {
            if !sp.is_empty() {
                messages.push(ChatMessage {
                    role: "system".to_string(),
                    content: Some(Value::String(sp)),
                    reasoning_content: None,
                    tool_call_id: None,
                    name: None,
                });
            }
        }
        Ok(Self {
            server,
            cfg,
            client,
            messages,
        })
    }

    pub fn clear(&mut self) {
        let sys = self
            .messages
            .first()
            .filter(|m| m.role == "system")
            .cloned();
        self.messages.clear();
        if let Some(s) = sys {
            self.messages.push(s);
        }
    }

    fn auth_headers(&self, req: reqwest::blocking::RequestBuilder) -> reqwest::blocking::RequestBuilder {
        if let Some(k) = self.server.api_key.clone() {
            req.bearer_auth(k)
        } else {
            req
        }
    }

    pub fn chat_completions(&mut self, user_prompt: &str, extra: Option<Value>) -> Result<Value, Box<dyn std::error::Error>> {
        self.messages.push(ChatMessage {
            role: "user".to_string(),
            content: Some(Value::String(user_prompt.to_string())),
            reasoning_content: None,
            tool_call_id: None,
            name: None,
        });

        let mut body = json!({
            "model": self.cfg.model,
            "messages": self.messages,
            "stream": false
        });
        if let Some(Value::Object(map)) = extra {
            if let Value::Object(bm) = &mut body {
                for (k, v) in map {
                    bm.insert(k, v);
                }
            }
        }

        let url = endpoint_join(self.server.base_url.trim_end_matches('/'), "/v1/chat/completions");
        let req = self.client.post(url).json(&body);
        let req = self.auth_headers(req);
        let out: Value = req.send()?.error_for_status()?.json()?;
        if let Some(msg) = out
            .get("choices")
            .and_then(|c| c.get(0))
            .and_then(|c0| c0.get("message"))
        {
            if let Ok(m) = serde_json::from_value::<ChatMessage>(msg.clone()) {
                self.messages.push(m);
            }
        }
        Ok(out)
    }

    pub fn chat_completions_stream(
        &mut self,
        user_prompt: &str,
        mut on_delta: Option<impl FnMut(&str)>,
        extra: Option<Value>,
    ) -> Result<StreamResult, Box<dyn std::error::Error>> {
        self.messages.push(ChatMessage {
            role: "user".to_string(),
            content: Some(Value::String(user_prompt.to_string())),
            reasoning_content: None,
            tool_call_id: None,
            name: None,
        });

        let mut body = json!({
            "model": self.cfg.model,
            "messages": self.messages,
            "stream": true
        });
        if let Some(Value::Object(map)) = extra {
            if let Value::Object(bm) = &mut body {
                for (k, v) in map {
                    bm.insert(k, v);
                }
            }
        }

        let url = endpoint_join(self.server.base_url.trim_end_matches('/'), "/v1/chat/completions");
        let req = self.client.post(url).json(&body);
        let req = self.auth_headers(req);
        let resp = req.send()?.error_for_status()?;

        let mut content_parts: Vec<String> = Vec::new();
        let mut reasoning_parts: Vec<String> = Vec::new();
        let mut tool_calls_by_index: BTreeMap<i64, Value> = BTreeMap::new();
        let mut usage: Option<Value> = None;

        let reader = BufReader::new(resp);
        for line in reader.lines() {
            let line = line.unwrap_or_default();
            let line = line.trim_end_matches('\r');
            if !line.starts_with("data:") {
                continue;
            }
            let data = line.trim_start_matches("data:").trim();
            if data.is_empty() {
                continue;
            }
            if data == "[DONE]" {
                break;
            }
            let chunk: Value = match serde_json::from_str(data) {
                Ok(v) => v,
                Err(_) => continue,
            };
            if let Some(u) = chunk.get("usage") {
                usage = Some(u.clone());
            }
            let delta = chunk
                .get("choices")
                .and_then(|c| c.get(0))
                .and_then(|c0| c0.get("delta"));
            let Some(delta) = delta else { continue };

            if let Some(c) = delta.get("content").and_then(|v| v.as_str()) {
                if !c.is_empty() {
                    content_parts.push(c.to_string());
                    if let Some(cb) = &mut on_delta {
                        cb(c);
                    }
                }
            }
            if let Some(r) = delta.get("reasoning_content").and_then(|v| v.as_str()) {
                if !r.is_empty() {
                    reasoning_parts.push(r.to_string());
                }
            }
            if let Some(tcs) = delta.get("tool_calls").and_then(|v| v.as_array()) {
                for tc in tcs {
                    let idx = tc.get("index").and_then(|v| v.as_i64()).unwrap_or(0);
                    let acc = tool_calls_by_index.entry(idx).or_insert_with(|| {
                        json!({
                            "id": null,
                            "function": {
                                "name": null,
                                "arguments": ""
                            }
                        })
                    });
                    if let Some(id) = tc.get("id").and_then(|v| v.as_str()) {
                        acc["id"] = Value::String(id.to_string());
                    }
                    if let Some(fn_obj) = tc.get("function") {
                        if let Some(name) = fn_obj.get("name").and_then(|v| v.as_str()) {
                            acc["function"]["name"] = Value::String(name.to_string());
                        }
                        if let Some(args) = fn_obj.get("arguments").and_then(|v| v.as_str()) {
                            let prev = acc["function"]["arguments"].as_str().unwrap_or("");
                            acc["function"]["arguments"] = Value::String(format!("{}{}", prev, args));
                        }
                    }
                }
            }
        }

        let content = content_parts.join("");
        let reasoning = reasoning_parts.join("");
        self.messages.push(ChatMessage {
            role: "assistant".to_string(),
            content: Some(Value::String(content.clone())),
            reasoning_content: if reasoning.is_empty() {
                None
            } else {
                Some(Value::String(reasoning.clone()))
            },
            tool_call_id: None,
            name: None,
        });

        Ok(StreamResult {
            usage,
            content,
            reasoning,
            tool_calls_by_index,
        })
    }
}

