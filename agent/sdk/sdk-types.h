#pragma once

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace llama_agent_sdk {

using json = nlohmann::ordered_json;

enum class stop_reason {
  COMPLETED,
  MAX_ITERATIONS,
  USER_CANCELLED,
  ERROR,
};

struct session_stats {
  int32_t total_input = 0;
  int32_t total_output = 0;
  int32_t total_cached = 0;

  double total_prompt_ms = 0;
  double total_predicted_ms = 0;

  int32_t subagent_input = 0;
  int32_t subagent_output = 0;
  int32_t subagent_cached = 0;
  int32_t subagent_count = 0;
};

enum class event_type {
  TEXT_DELTA,
  REASONING_DELTA,
  TOOL_START,
  TOOL_RESULT,
  PERMISSION_REQUIRED,
  PERMISSION_RESOLVED,
  ITERATION_START,
  COMPLETED,
  ERROR,
};

struct event {
  event_type type;
  json data;
};

using event_callback = std::function<void(const event &)>;

struct run_result {
  stop_reason reason = stop_reason::ERROR;
  std::string final_response;
  int iterations = 0;
};

} // namespace llama_agent_sdk
