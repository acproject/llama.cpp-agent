from __future__ import annotations

import dataclasses
import json
import ssl
import urllib.parse
import urllib.request
from typing import Any, Callable, Dict, Generator, List, Optional, Tuple


Json = Dict[str, Any]


@dataclasses.dataclass
class HttpServerConfig:
    base_url: str
    api_key: str = ""


@dataclasses.dataclass
class HttpAgentConfig:
    model: str
    working_dir: str = "."
    system_prompt: str = ""
    max_iterations: int = 50
    request_timeout_s: int = 300


def _endpoint_join(prefix: str, suffix: str) -> str:
    if not prefix or prefix == "/":
        return suffix
    if not suffix or suffix == "/":
        return prefix
    if prefix.endswith("/") and suffix.startswith("/"):
        return prefix + suffix[1:]
    if (not prefix.endswith("/")) and (not suffix.startswith("/")):
        return prefix + "/" + suffix
    return prefix + suffix


def _iter_sse_lines(resp) -> Generator[str, None, None]:
    for raw in resp:
        try:
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        except Exception:
            continue
        yield line


def _iter_sse_data_events(resp) -> Generator[str, None, None]:
    for line in _iter_sse_lines(resp):
        if not line.startswith("data:"):
            continue
        data = line[len("data:") :].strip()
        if not data:
            continue
        if data == "[DONE]":
            break
        yield data


def _accumulate_chat_completions_stream(resp) -> Tuple[str, str, Dict[int, Dict[str, Any]], Optional[Json]]:
    content_parts: List[str] = []
    reasoning_parts: List[str] = []
    tool_calls_by_index: Dict[int, Dict[str, Any]] = {}
    usage: Optional[Json] = None

    for data in _iter_sse_data_events(resp):
        chunk = json.loads(data)
        if "usage" in chunk and isinstance(chunk["usage"], dict):
            usage = chunk["usage"]
        choices = chunk.get("choices") or []
        if not choices:
            continue
        delta = (choices[0].get("delta") or {})

        c = delta.get("content")
        if isinstance(c, str) and c:
            content_parts.append(c)

        r = delta.get("reasoning_content")
        if isinstance(r, str) and r:
            reasoning_parts.append(r)

        for tc in (delta.get("tool_calls") or []):
            try:
                idx = int(tc.get("index"))
            except Exception:
                continue
            acc = tool_calls_by_index.setdefault(idx, {"id": None, "function": {"name": None, "arguments": ""}})
            if isinstance(tc.get("id"), str):
                acc["id"] = tc["id"]
            fn = tc.get("function") or {}
            if isinstance(fn.get("name"), str):
                acc["function"]["name"] = fn["name"]
            if isinstance(fn.get("arguments"), str):
                acc["function"]["arguments"] += fn["arguments"]

    return "".join(content_parts), "".join(reasoning_parts), tool_calls_by_index, usage


class HttpAgentSession:
    def __init__(self, server: HttpServerConfig, config: HttpAgentConfig):
        self._server = server
        self._config = config
        self._messages: List[Json] = []
        if config.system_prompt:
            self._messages.append({"role": "system", "content": config.system_prompt})

    @property
    def messages(self) -> List[Json]:
        return self._messages

    def clear(self) -> None:
        sys = self._messages[0] if self._messages and self._messages[0].get("role") == "system" else None
        self._messages = []
        if sys:
            self._messages.append(sys)

    def _headers(self) -> Dict[str, str]:
        h = {"Content-Type": "application/json"}
        if self._server.api_key:
            h["Authorization"] = f"Bearer {self._server.api_key}"
        return h

    def _request(self, path: str, body: Json, stream: bool):
        url = _endpoint_join(self._server.base_url.rstrip("/"), path)
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(url=url, data=data, headers=self._headers(), method="POST")
        ctx = ssl.create_default_context()
        return urllib.request.urlopen(req, timeout=self._config.request_timeout_s, context=ctx)

    def chat_completions(self, user_prompt: str, extra: Optional[Json] = None) -> Json:
        self._messages.append({"role": "user", "content": user_prompt})
        body: Json = {"model": self._config.model, "messages": self._messages, "stream": False}
        if extra:
            body.update(extra)
        with self._request("/v1/chat/completions", body, stream=False) as resp:
            raw = resp.read()
        out = json.loads(raw.decode("utf-8", errors="replace"))
        msg = (((out.get("choices") or [{}])[0]).get("message") or {})
        if msg:
            self._messages.append(msg)
        return out

    def chat_completions_stream(
        self,
        user_prompt: str,
        on_delta: Optional[Callable[[str], None]] = None,
        extra: Optional[Json] = None,
    ) -> Tuple[Json, str, str, Dict[int, Dict[str, Any]]]:
        self._messages.append({"role": "user", "content": user_prompt})
        body: Json = {"model": self._config.model, "messages": self._messages, "stream": True}
        if extra:
            body.update(extra)

        url = _endpoint_join(self._server.base_url.rstrip("/"), "/v1/chat/completions")
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(url=url, data=data, headers=self._headers(), method="POST")
        ctx = ssl.create_default_context()

        content_parts: List[str] = []
        reasoning_parts: List[str] = []
        tool_calls_by_index: Dict[int, Dict[str, Any]] = {}
        usage: Optional[Json] = None

        with urllib.request.urlopen(req, timeout=self._config.request_timeout_s, context=ctx) as resp:
            for data_line in _iter_sse_data_events(resp):
                chunk = json.loads(data_line)
                if "usage" in chunk and isinstance(chunk["usage"], dict):
                    usage = chunk["usage"]
                choices = chunk.get("choices") or []
                if not choices:
                    continue
                delta = (choices[0].get("delta") or {})
                c = delta.get("content")
                if isinstance(c, str) and c:
                    content_parts.append(c)
                    if on_delta:
                        on_delta(c)
                r = delta.get("reasoning_content")
                if isinstance(r, str) and r:
                    reasoning_parts.append(r)
                for tc in (delta.get("tool_calls") or []):
                    try:
                        idx = int(tc.get("index"))
                    except Exception:
                        continue
                    acc = tool_calls_by_index.setdefault(
                        idx, {"id": None, "function": {"name": None, "arguments": ""}}
                    )
                    if isinstance(tc.get("id"), str):
                        acc["id"] = tc["id"]
                    fn = tc.get("function") or {}
                    if isinstance(fn.get("name"), str):
                        acc["function"]["name"] = fn["name"]
                    if isinstance(fn.get("arguments"), str):
                        acc["function"]["arguments"] += fn["arguments"]

        content = "".join(content_parts)
        reasoning = "".join(reasoning_parts)
        assistant_msg: Json = {"role": "assistant", "content": content}
        if reasoning:
            assistant_msg["reasoning_content"] = reasoning
        if tool_calls_by_index:
            tool_calls = []
            for idx in sorted(tool_calls_by_index.keys()):
                tc = tool_calls_by_index[idx]
                tool_calls.append(
                    {
                        "id": tc.get("id") or f"call_{idx}",
                        "type": "function",
                        "function": {
                            "name": (tc.get("function") or {}).get("name") or "",
                            "arguments": (tc.get("function") or {}).get("arguments") or "",
                        },
                    }
                )
            assistant_msg["tool_calls"] = tool_calls
        self._messages.append(assistant_msg)
        if usage:
            return {"usage": usage}, content, reasoning, tool_calls_by_index
        return {}, content, reasoning, tool_calls_by_index
