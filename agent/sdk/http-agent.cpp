#include "http-agent.h"
#include "mcp-manager.h"
#include "prompt-builder.h"

#include "../../third_party/llama.cpp/common/http.h"

#include <cpp-httplib/httplib.h>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace llama_agent_sdk {

static std::string endpoint_join(const std::string &prefix,
                                 const std::string &suffix) {
  if (prefix.empty() || prefix == "/") {
    return suffix;
  }
  if (suffix.empty() || suffix == "/") {
    return prefix;
  }
  if (prefix.back() == '/' && suffix.front() == '/') {
    return prefix + suffix.substr(1);
  }
  if (prefix.back() != '/' && suffix.front() != '/') {
    return prefix + "/" + suffix;
  }
  return prefix + suffix;
}

static void set_timeout_ms(httplib::Client &cli, int timeout_ms) {
  int sec = timeout_ms / 1000;
  int usec = (timeout_ms % 1000) * 1000;
  cli.set_connection_timeout(sec, usec);
  cli.set_read_timeout(sec, usec);
  cli.set_write_timeout(sec, usec);
}

http_agent_session::http_agent_session(const http_server_config &server,
                                       const http_agent_config &config)
    : server_(server), config_(config) {
  tool_ctx_.working_dir = config_.working_dir.empty() ? "." : config_.working_dir;
  tool_ctx_.timeout_ms = config_.tool_timeout_ms;
  tool_ctx_.is_interrupted = nullptr;
  tool_ctx_.server_ctx_ptr = this;

  if (!config_.working_dir.empty()) {
    permissions_.set_project_root(config_.working_dir);
  }
  permissions_.set_yolo_mode(config_.yolo_mode);

  if (config_.enable_mcp) {
    ensure_mcp_tools(tool_ctx_.working_dir);
  }

  prompt_build_options opts;
  opts.enable_skills = config_.enable_skills;
  opts.extra_skills_paths = config_.extra_skills_paths;
  opts.enable_agents_md = config_.enable_agents_md;
  prompt_build_result built =
      build_system_prompt(config_.system_prompt, tool_ctx_.working_dir, opts);
  config_.system_prompt = built.system_prompt;

  init_system_message();
}

void http_agent_session::init_system_message() {
  if (!messages_.empty()) {
    return;
  }
  json sys = {{"role", "system"}, {"content", config_.system_prompt}};
  messages_.push_back(sys);
  tool_ctx_.base_system_prompt = config_.system_prompt;
}

void http_agent_session::clear() {
  messages_.clear();
  stats_ = session_stats{};
  permissions_.clear_session();
  init_system_message();
}

json http_agent_session::build_tools_json() const {
  const auto &registry = tool_registry::instance();
  std::vector<common_chat_tool> tools =
      config_.allowed_tools.empty()
          ? registry.to_chat_tools()
          : registry.to_chat_tools_filtered(config_.allowed_tools);

  json out = json::array();
  for (const auto &t : tools) {
    json params = json::object();
    try {
      if (!t.parameters.empty()) {
        params = json::parse(t.parameters);
      }
    } catch (...) {
      params = json::object();
    }
    out.push_back({
        {"type", "function"},
        {"function",
         {{"name", t.name}, {"description", t.description}, {"parameters", params}}},
    });
  }
  return out;
}

json http_agent_session::build_request_body() const {
  json body;
  body["model"] = config_.model;
  body["messages"] = messages_;
  json tools = build_tools_json();
  if (!tools.empty()) {
    body["tools"] = tools;
    body["tool_choice"] = "auto";
  }
  return body;
}

static void acc_from_message(const json &msg,
                             http_assistant_accumulator &acc) {
  if (msg.contains("content") && msg["content"].is_string()) {
    acc.content = msg["content"].get<std::string>();
  }
  if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string()) {
    acc.reasoning_content = msg["reasoning_content"].get<std::string>();
  }
  if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
    for (const auto &tc : msg["tool_calls"]) {
      if (!tc.is_object()) {
        continue;
      }
      int index = -1;
      if (tc.contains("index") && tc["index"].is_number_integer()) {
        index = tc["index"].get<int>();
      }
      if (index < 0) {
        index = static_cast<int>(acc.tool_calls_by_index.size());
      }
      auto &dst = acc.tool_calls_by_index[index];
      if (tc.contains("id") && tc["id"].is_string()) {
        dst.id = tc["id"].get<std::string>();
      }
      if (tc.contains("function") && tc["function"].is_object()) {
        const auto &fn = tc["function"];
        if (fn.contains("name") && fn["name"].is_string()) {
          dst.name = fn["name"].get<std::string>();
        }
        if (fn.contains("arguments") && fn["arguments"].is_string()) {
          dst.arguments = fn["arguments"].get<std::string>();
        }
      }
    }
  }
}

bool http_agent_session::post_chat_completions(
    const json &body, http_assistant_accumulator &acc, std::optional<json> &out_usage,
    std::string &out_error) {
  auto [cli, parts] = common_http_client(server_.base_url);
  set_timeout_ms(cli, config_.request_timeout_ms);

  httplib::Headers headers;
  headers.emplace("Accept", "application/json");
  if (!server_.api_key.empty()) {
    headers.emplace("Authorization", "Bearer " + server_.api_key);
  }

  std::string endpoint = endpoint_join(parts.path, "/v1/chat/completions");

  auto res = cli.Post(endpoint, headers, body.dump(),
                      "application/json; charset=utf-8");
  if (!res) {
    out_error = "HTTP error: " + std::to_string(static_cast<int>(res.error()));
    return false;
  }
  if (res->status < 200 || res->status >= 300) {
    out_error = res->body;
    return false;
  }

  json parsed;
  try {
    parsed = json::parse(res->body);
  } catch (const std::exception &e) {
    out_error = std::string("Invalid JSON response: ") + e.what();
    return false;
  }

  if (parsed.contains("usage") && parsed["usage"].is_object()) {
    out_usage = parsed["usage"];
  }
  if (!parsed.contains("choices") || !parsed["choices"].is_array() ||
      parsed["choices"].empty()) {
    out_error = "Missing choices in response";
    return false;
  }
  const auto &choice0 = parsed["choices"][0];
  if (!choice0.contains("message") || !choice0["message"].is_object()) {
    out_error = "Missing message in response";
    return false;
  }

  acc_from_message(choice0["message"], acc);
  return true;
}

static bool parse_sse_lines(const char *data, size_t len, std::string &buffer,
                            std::function<bool(const std::string &)> on_data) {
  buffer.append(data, len);
  for (;;) {
    auto pos = buffer.find('\n');
    if (pos == std::string::npos) {
      return true;
    }
    std::string line = buffer.substr(0, pos);
    buffer.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("data: ", 0) == 0) {
      std::string payload = line.substr(6);
      if (!on_data(payload)) {
        return false;
      }
    }
  }
}

static void process_chunk_delta(const json &delta,
                                http_assistant_accumulator &acc,
                                const event_callback &on_event) {
  if (delta.contains("content") && delta["content"].is_string()) {
    std::string d = delta["content"].get<std::string>();
    if (!d.empty()) {
      acc.content += d;
      if (on_event) {
        on_event({event_type::TEXT_DELTA, {{"content", d}}});
      }
    }
  }
  if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
    std::string d = delta["reasoning_content"].get<std::string>();
    if (!d.empty()) {
      acc.reasoning_content += d;
      if (on_event) {
        on_event({event_type::REASONING_DELTA, {{"content", d}}});
      }
    }
  }
  if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
    for (const auto &tc : delta["tool_calls"]) {
      if (!tc.is_object()) {
        continue;
      }
      int index = tc.value("index", -1);
      if (index < 0) {
        continue;
      }
      auto &dst = acc.tool_calls_by_index[index];
      if (tc.contains("id") && tc["id"].is_string()) {
        dst.id = tc["id"].get<std::string>();
      }
      if (tc.contains("function") && tc["function"].is_object()) {
        const auto &fn = tc["function"];
        if (fn.contains("name") && fn["name"].is_string()) {
          dst.name = fn["name"].get<std::string>();
        }
        if (fn.contains("arguments") && fn["arguments"].is_string()) {
          dst.arguments += fn["arguments"].get<std::string>();
        }
      }
    }
  }
}

static void process_chunk_json(
    const json &chunk, http_assistant_accumulator &acc,
    const event_callback &on_event, std::optional<json> &out_usage) {
  if (chunk.contains("usage") && chunk["usage"].is_object()) {
    out_usage = chunk["usage"];
  }
  if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
      chunk["choices"].empty()) {
    return;
  }
  const auto &choice0 = chunk["choices"][0];
  if (!choice0.contains("delta") || !choice0["delta"].is_object()) {
    return;
  }
  process_chunk_delta(choice0["delta"], acc, on_event);
}

bool http_agent_session::post_chat_completions_streaming(
    const json &body, event_callback on_event, std::function<bool()> should_stop,
    http_assistant_accumulator &acc, std::optional<json> &out_usage,
    std::string &out_error) {
  auto [cli, parts] = common_http_client(server_.base_url);
  set_timeout_ms(cli, config_.request_timeout_ms);

  httplib::Headers headers;
  headers.emplace("Accept", "text/event-stream");
  if (!server_.api_key.empty()) {
    headers.emplace("Authorization", "Bearer " + server_.api_key);
  }

  std::string endpoint = endpoint_join(parts.path, "/v1/chat/completions");

  std::string sse_buf;
  std::string recv_error;

  auto receiver = [&](const char *data, size_t len) -> bool {
    if (should_stop && should_stop()) {
      recv_error = "Cancelled";
      return false;
    }
    return parse_sse_lines(data, len, sse_buf, [&](const std::string &payload) {
      if (payload == "[DONE]") {
        return true;
      }
      json parsed;
      try {
        parsed = json::parse(payload);
      } catch (...) {
        return true;
      }

      if (parsed.is_array()) {
        for (const auto &item : parsed) {
          if (item.is_object()) {
            process_chunk_json(item, acc, on_event, out_usage);
          }
        }
      } else if (parsed.is_object()) {
        process_chunk_json(parsed, acc, on_event, out_usage);
      }
      return true;
    });
  };

  auto res = cli.Post(endpoint, headers, body.dump(),
                      "application/json; charset=utf-8", receiver);

  if (!res) {
    out_error = recv_error.empty()
                    ? "HTTP error: " + std::to_string(static_cast<int>(res.error()))
                    : recv_error;
    return false;
  }
  if (res->status < 200 || res->status >= 300) {
    out_error = res->body;
    return false;
  }
  if (!recv_error.empty()) {
    out_error = recv_error;
    return false;
  }
  return true;
}

json http_agent_session::finalize_assistant_message(
    const http_assistant_accumulator &acc) const {
  json msg;
  msg["role"] = "assistant";
  msg["content"] = acc.content;

  if (!acc.reasoning_content.empty()) {
    msg["reasoning_content"] = acc.reasoning_content;
  }

  if (!acc.tool_calls_by_index.empty()) {
    json tcs = json::array();
    for (const auto &[idx, tc] : acc.tool_calls_by_index) {
      if (!tc.name.has_value()) {
        continue;
      }
      json fn;
      fn["name"] = *tc.name;
      fn["arguments"] = tc.arguments;

      json out;
      out["type"] = "function";
      out["function"] = fn;
      if (tc.id.has_value()) {
        out["id"] = *tc.id;
      } else {
        std::ostringstream ss;
        ss << "call_" << idx;
        out["id"] = ss.str();
      }
      tcs.push_back(out);
    }
    if (!tcs.empty()) {
      msg["tool_calls"] = tcs;
    }
  }

  return msg;
}

std::string http_agent_session::hash_args(const std::string &s) {
  std::hash<std::string> hasher;
  return std::to_string(hasher(s));
}

tool_result http_agent_session::execute_tool_call_async(
    const std::string &tool_name, const std::string &arguments_json,
    event_callback on_event, std::function<bool()> should_stop) {

  if (!config_.allowed_tools.empty() && !config_.allowed_tools.count(tool_name)) {
    return {false, "", "Tool not allowed: " + tool_name};
  }

  const auto &registry = tool_registry::instance();
  const tool_def *tool = registry.get_tool(tool_name);
  if (!tool) {
    return {false, "", "Unknown tool: " + tool_name};
  }

  json args;
  try {
    args = json::parse(arguments_json);
  } catch (const std::exception &e) {
    return {false, "", std::string("Invalid JSON arguments: ") + e.what()};
  }

  permission_type ptype = permission_type::BASH;
  if (tool_name == "read") {
    ptype = permission_type::FILE_READ;
  } else if (tool_name == "write") {
    ptype = permission_type::FILE_WRITE;
  } else if (tool_name == "edit") {
    ptype = permission_type::FILE_EDIT;
  } else if (tool_name == "glob") {
    ptype = permission_type::GLOB;
  }

  permission_request req;
  req.type = ptype;
  req.tool_name = tool_name;
  req.details = arguments_json;

  if (tool_name == "read" || tool_name == "write" || tool_name == "edit") {
    std::string file_path = args.value("file_path", "");
    if (!file_path.empty()) {
      fs::path path(file_path);
      if (path.is_relative()) {
        path = fs::path(tool_ctx_.working_dir) / path;
      }
      if (permissions_.is_external_path(path.string())) {
        permission_request ext_req;
        ext_req.type = permission_type::EXTERNAL_DIR;
        ext_req.tool_name = tool_name;
        ext_req.details = "External file: " + path.string();
        ext_req.is_dangerous = true;
        ext_req.description = "Operation outside working directory";

        std::string req_id = permissions_.request_permission(ext_req);
        if (on_event) {
          on_event({event_type::PERMISSION_REQUIRED,
                    {{"required_id", req_id},
                     {"tool", tool_name},
                     {"details", ext_req.details},
                     {"dangerous", true}}});
        }

        auto resp =
            permissions_.wait_for_response(req_id, config_.permission_timeout_ms);
        if (should_stop && should_stop()) {
          permissions_.cancel(req_id);
          return {false, "", "Operation cancelled"};
        }
        if (!resp || !resp->allowed) {
          if (on_event) {
            on_event({event_type::PERMISSION_RESOLVED,
                      {{"required_id", req_id}, {"allowed", false}}});
          }
          return {false, "", "Permission denied for external file"};
        }
        if (on_event) {
          on_event({event_type::PERMISSION_RESOLVED,
                    {{"required_id", req_id}, {"allowed", true}}});
        }
      }
    }
  }

  std::string args_hash = hash_args(arguments_json);
  if (permissions_.is_doom_loop(tool_name, args_hash)) {
    permission_request doom;
    doom.type = ptype;
    doom.tool_name = tool_name;
    doom.details = "Repeated identical calls";
    doom.is_dangerous = true;
    doom.description = "Blocked: Detected repeated identical tool calls";

    std::string req_id = permissions_.request_permission(doom);
    if (on_event) {
      on_event({event_type::PERMISSION_REQUIRED,
                {{"required_id", req_id},
                 {"tool", tool_name},
                 {"details", doom.details},
                 {"dangerous", true}}});
    }
    auto resp =
        permissions_.wait_for_response(req_id, config_.permission_timeout_ms);
    if (should_stop && should_stop()) {
      permissions_.cancel(req_id);
      return {false, "", "Operation cancelled"};
    }
    bool allowed = resp && resp->allowed;
    if (on_event) {
      on_event({event_type::PERMISSION_RESOLVED,
                {{"required_id", req_id}, {"allowed", allowed}}});
    }
    if (!allowed) {
      return {false, "", doom.description};
    }
  }

  permission_state state = permissions_.check_permission(req);
  if (state == permission_state::DENY || state == permission_state::DENY_SESSION) {
    return {false, "", "Permission denied for " + tool_name};
  }
  if (state == permission_state::ASK) {
    std::string req_id = permissions_.request_permission(req);
    if (on_event) {
      on_event({event_type::PERMISSION_REQUIRED,
                {{"required_id", req_id},
                 {"tool", tool_name},
                 {"details", req.details},
                 {"dangerous", req.is_dangerous}}});
    }
    auto resp =
        permissions_.wait_for_response(req_id, config_.permission_timeout_ms);
    if (should_stop && should_stop()) {
      permissions_.cancel(req_id);
      return {false, "", "Operation cancelled"};
    }
    bool allowed = resp && resp->allowed;
    if (on_event) {
      on_event({event_type::PERMISSION_RESOLVED,
                {{"required_id", req_id}, {"allowed", allowed}}});
    }
    if (!allowed) {
      return {false, "", "User denied permission for " + tool_name};
    }
  }

  permissions_.record_tool_call(tool_name, args_hash);

  if (on_event) {
    on_event({event_type::TOOL_START, {{"name", tool_name}, {"args", arguments_json}}});
  }

  auto start = std::chrono::steady_clock::now();
  tool_ctx_.is_interrupted = nullptr;
  tool_result result = config_.bash_patterns.empty()
                           ? registry.execute(tool_name, args, tool_ctx_)
                           : registry.execute_filtered(tool_name, args, tool_ctx_,
                                                      config_.bash_patterns);
  auto end = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  if (on_event) {
    std::string out = result.success ? result.output : result.error;
    on_event({event_type::TOOL_RESULT,
              {{"name", tool_name},
               {"success", result.success},
               {"output", out},
               {"duration_ms", elapsed}}});
  }

  return result;
}

void http_agent_session::add_tool_result_message(const std::string &tool_name,
                                                 const std::string &call_id,
                                                 const tool_result &result) {
  std::string content = result.success ? result.output : ("ERROR: " + result.error);
  json msg = {{"role", "tool"},
              {"tool_call_id", call_id},
              {"name", tool_name},
              {"content", content}};
  messages_.push_back(msg);
}

std::vector<permission_request_async> http_agent_session::pending_permissions() {
  return permissions_.pending();
}

bool http_agent_session::respond_permission(const std::string &request_id,
                                            bool allow, permission_scope scope) {
  return permissions_.respond(request_id, allow, scope);
}

void http_agent_session::add_subagent_stats(const session_stats &child) {
  stats_.subagent_input += child.total_input;
  stats_.subagent_output += child.total_output;
  stats_.subagent_cached += child.total_cached;
  stats_.subagent_count += 1;

  stats_.total_input += child.total_input;
  stats_.total_output += child.total_output;
  stats_.total_cached += child.total_cached;

  stats_.total_prompt_ms += child.total_prompt_ms;
  stats_.total_predicted_ms += child.total_predicted_ms;
}

run_result http_agent_session::run(const std::string &user_prompt,
                                   std::function<bool()> should_stop) {
  run_result out;
  out.reason = stop_reason::ERROR;

  if (should_stop && should_stop()) {
    out.reason = stop_reason::USER_CANCELLED;
    return out;
  }

  init_system_message();
  messages_.push_back({{"role", "user"}, {"content", user_prompt}});

  std::string last_content;

  for (int i = 0; i < config_.max_iterations; ++i) {
    if (should_stop && should_stop()) {
      out.reason = stop_reason::USER_CANCELLED;
      out.final_response = last_content;
      out.iterations = i;
      return out;
    }

    json body = build_request_body();
    body["stream"] = false;

    http_assistant_accumulator acc;
    std::optional<json> usage;
    std::string err;
    if (!post_chat_completions(body, acc, usage, err)) {
      out.reason = stop_reason::ERROR;
      out.final_response = err;
      out.iterations = i;
      return out;
    }

    json assistant_msg = finalize_assistant_message(acc);
    messages_.push_back(assistant_msg);

    if (usage && usage->is_object()) {
      stats_.total_input += usage->value("prompt_tokens", 0);
      stats_.total_output += usage->value("completion_tokens", 0);
    }

    last_content = acc.content;

    if (!assistant_msg.contains("tool_calls") || !assistant_msg["tool_calls"].is_array() ||
        assistant_msg["tool_calls"].empty()) {
      out.reason = stop_reason::COMPLETED;
      out.final_response = acc.content;
      out.iterations = i + 1;
      return out;
    }

    for (const auto &tc : assistant_msg["tool_calls"]) {
      if (should_stop && should_stop()) {
        out.reason = stop_reason::USER_CANCELLED;
        out.final_response = last_content;
        out.iterations = i + 1;
        return out;
      }
      std::string call_id = tc.value("id", "");
      std::string tool_name;
      std::string args_json;
      if (tc.contains("function") && tc["function"].is_object()) {
        tool_name = tc["function"].value("name", "");
        args_json = tc["function"].value("arguments", "");
      }
      if (tool_name.empty()) {
        add_tool_result_message(tool_name, call_id,
                                {false, "", "Missing tool name"});
        continue;
      }
      tool_result tr =
          execute_tool_call_async(tool_name, args_json, nullptr, should_stop);
      add_tool_result_message(tool_name, call_id, tr);
    }
  }

  out.reason = stop_reason::MAX_ITERATIONS;
  out.final_response = last_content;
  out.iterations = config_.max_iterations;
  return out;
}

run_result http_agent_session::run_streaming(
    const std::string &user_prompt, event_callback on_event,
    std::function<bool()> should_stop) {
  run_result out;
  out.reason = stop_reason::ERROR;

  if (should_stop && should_stop()) {
    out.reason = stop_reason::USER_CANCELLED;
    return out;
  }

  init_system_message();
  messages_.push_back({{"role", "user"}, {"content", user_prompt}});

  std::string last_content;

  for (int i = 0; i < config_.max_iterations; ++i) {
    if (on_event) {
      on_event({event_type::ITERATION_START,
                {{"iteration", i + 1}, {"max_iterations", config_.max_iterations}}});
    }

    json body = build_request_body();
    body["stream"] = true;
    if (config_.include_usage) {
      body["stream_options"] = {{"include_usage", true}};
    }

    http_assistant_accumulator acc;
    std::optional<json> usage;
    std::string err;
    if (!post_chat_completions_streaming(body, on_event, should_stop, acc, usage, err)) {
      if (on_event) {
        on_event({event_type::ERROR, {{"message", err}}});
      }
      out.reason = err == "Cancelled" ? stop_reason::USER_CANCELLED : stop_reason::ERROR;
      out.final_response = err;
      out.iterations = i;
      return out;
    }

    json assistant_msg = finalize_assistant_message(acc);
    messages_.push_back(assistant_msg);

    if (usage && usage->is_object()) {
      stats_.total_input += usage->value("prompt_tokens", 0);
      stats_.total_output += usage->value("completion_tokens", 0);
    }

    last_content = acc.content;

    if (!assistant_msg.contains("tool_calls") || !assistant_msg["tool_calls"].is_array() ||
        assistant_msg["tool_calls"].empty()) {
      out.reason = stop_reason::COMPLETED;
      out.final_response = acc.content;
      out.iterations = i + 1;
      if (on_event) {
        on_event({event_type::COMPLETED,
                  {{"reason", "completed"},
                   {"stats",
                    {{"input_tokens", stats_.total_input},
                     {"output_tokens", stats_.total_output},
                     {"cached_tokens", stats_.total_cached}}}}});
      }
      return out;
    }

    for (const auto &tc : assistant_msg["tool_calls"]) {
      if (should_stop && should_stop()) {
        out.reason = stop_reason::USER_CANCELLED;
        out.final_response = last_content;
        out.iterations = i + 1;
        if (on_event) {
          on_event({event_type::COMPLETED,
                    {{"reason", "user_cancelled"},
                     {"stats",
                      {{"input_tokens", stats_.total_input},
                       {"output_tokens", stats_.total_output},
                       {"cached_tokens", stats_.total_cached}}}}});
        }
        return out;
      }
      std::string call_id = tc.value("id", "");
      std::string tool_name;
      std::string args_json;
      if (tc.contains("function") && tc["function"].is_object()) {
        tool_name = tc["function"].value("name", "");
        args_json = tc["function"].value("arguments", "");
      }
      if (tool_name.empty()) {
        tool_result tr{false, "", "Missing tool name"};
        add_tool_result_message(tool_name, call_id, tr);
        continue;
      }
      tool_result tr =
          execute_tool_call_async(tool_name, args_json, on_event, should_stop);
      add_tool_result_message(tool_name, call_id, tr);
    }
  }

  out.reason = stop_reason::MAX_ITERATIONS;
  out.final_response = last_content;
  out.iterations = config_.max_iterations;
  if (on_event) {
    on_event({event_type::COMPLETED,
              {{"reason", "max_iterations"},
               {"stats",
                {{"input_tokens", stats_.total_input},
                 {"output_tokens", stats_.total_output},
                 {"cached_tokens", stats_.total_cached}}}}});
  }
  return out;
}

} // namespace llama_agent_sdk
