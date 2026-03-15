#pragma once

#include "sdk-types.h"

#include "../permission-async.h"
#include "../tool-registry.h"

#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llama_agent_sdk {

struct http_assistant_accumulator {
  std::string content;
  std::string reasoning_content;
  struct tool_call_acc {
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::string arguments;
  };
  std::map<int, tool_call_acc> tool_calls_by_index;
};

struct http_server_config {
  std::string base_url;
  std::string api_key;
};

struct http_agent_config {
  std::string model;
  std::string working_dir = ".";
  bool yolo_mode = false;
  int max_iterations = 50;
  int tool_timeout_ms = 120000;
  int request_timeout_ms = 300000;
  int permission_timeout_ms = 300000;
  bool include_usage = true;

  std::set<std::string> allowed_tools;
  std::set<std::string> bash_patterns;

  std::string system_prompt;

  bool enable_skills = true;
  std::vector<std::string> extra_skills_paths;

  bool enable_agents_md = true;

  bool enable_mcp = true;
};

class http_agent_session {
public:
  http_agent_session(const http_server_config &server,
                     const http_agent_config &config);

  const json &messages() const { return messages_; }
  const session_stats &stats() const { return stats_; }
  const http_server_config &server() const { return server_; }
  const http_agent_config &config() const { return config_; }

  void clear();

  run_result run(const std::string &user_prompt,
                 std::function<bool()> should_stop = nullptr);

  run_result run_streaming(const std::string &user_prompt,
                           event_callback on_event,
                           std::function<bool()> should_stop = nullptr);

  std::vector<permission_request_async> pending_permissions();

  bool respond_permission(const std::string &request_id, bool allow,
                          permission_scope scope = permission_scope::ONCE);

  void add_subagent_stats(const session_stats &child);

private:
  http_server_config server_;
  http_agent_config config_;

  json messages_ = json::array();
  session_stats stats_;
  tool_context tool_ctx_;

  permission_manager_async permissions_;

  void init_system_message();

  json build_tools_json() const;
  json build_request_body() const;

  bool post_chat_completions_streaming(
      const json &body, event_callback on_event, std::function<bool()> should_stop,
      http_assistant_accumulator &acc, std::optional<json> &out_usage,
      std::string &out_error);

  bool post_chat_completions(
      const json &body, http_assistant_accumulator &acc, std::optional<json> &out_usage,
      std::string &out_error);

  json finalize_assistant_message(const http_assistant_accumulator &acc) const;

  tool_result execute_tool_call_async(const std::string &tool_name,
                                      const std::string &arguments_json,
                                      event_callback on_event,
                                      std::function<bool()> should_stop);

  void add_tool_result_message(const std::string &tool_name,
                               const std::string &call_id,
                               const tool_result &result);

  static std::string hash_args(const std::string &s);
};

} // namespace llama_agent_sdk
