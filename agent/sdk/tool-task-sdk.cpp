#include "../tool-registry.h"
#include "../subagent/subagent-types.h"

#include "http-agent.h"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = nlohmann::ordered_json;

namespace {

struct sdk_subagent_task {
  std::string id;
  std::thread thread;
  std::promise<tool_result> promise;
  std::atomic<bool> complete{false};
};

class sdk_subagent_runner {
public:
  explicit sdk_subagent_runner(llama_agent_sdk::http_agent_session &parent)
      : parent_(parent) {}

  std::string start_background(const subagent_type type,
                               const std::string &prompt) {
    std::string task_id = generate_task_id();
    auto task = std::make_unique<sdk_subagent_task>();
    task->id = task_id;
    auto *task_ptr = task.get();

    task->thread = std::thread([this, task_ptr, type, prompt] {
      tool_result result;
      try {
        result = run(type, prompt);
      } catch (const std::exception &e) {
        result.success = false;
        result.error = std::string("Exception: ") + e.what();
      }
      task_ptr->promise.set_value(result);
      task_ptr->complete.store(true);
    });

    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_[task_id] = std::move(task);
    }
    return task_id;
  }

  bool is_complete(const std::string &task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
      return false;
    }
    return it->second->complete.load();
  }

  tool_result get_result(const std::string &task_id) {
    std::unique_ptr<sdk_subagent_task> task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = tasks_.find(task_id);
      if (it == tasks_.end()) {
        return {false, "", "Task not found: " + task_id};
      }
      if (!it->second->complete.load()) {
        return {true, "Task " + task_id + " is still running.", ""};
      }
      task = std::move(it->second);
      tasks_.erase(it);
    }

    tool_result result;
    try {
      result = task->promise.get_future().get();
    } catch (const std::exception &e) {
      result.success = false;
      result.error = std::string("Failed to get result: ") + e.what();
    }

    if (task->thread.joinable()) {
      task->thread.join();
    }
    return result;
  }

  tool_result run(const subagent_type type, const std::string &prompt) {
    const auto &cfg = get_subagent_config(type);

    llama_agent_sdk::http_agent_config child_cfg = parent_.config();
    child_cfg.max_iterations = cfg.max_iterations;
    child_cfg.allowed_tools = cfg.allowed_tools;
    child_cfg.bash_patterns =
        std::set<std::string>(cfg.bash_patterns.begin(), cfg.bash_patterns.end());

    child_cfg.system_prompt = build_system_prompt(cfg);
    child_cfg.enable_skills = false;
    child_cfg.extra_skills_paths.clear();
    child_cfg.enable_agents_md = false;
    child_cfg.enable_mcp = false;

    llama_agent_sdk::http_agent_session child(parent_.server(), child_cfg);
    auto res = child.run(prompt);

    parent_.add_subagent_stats(child.stats());

    if (res.reason == llama_agent_sdk::stop_reason::COMPLETED ||
        res.reason == llama_agent_sdk::stop_reason::MAX_ITERATIONS) {
      tool_result tr;
      tr.success = (res.reason == llama_agent_sdk::stop_reason::COMPLETED);
      tr.output = res.final_response;
      if (!tr.success) {
        tr.error = "Reached maximum iterations (" +
                   std::to_string(cfg.max_iterations) + ")";
      }
      return tr;
    }

    tool_result tr;
    tr.success = false;
    tr.error =
        res.reason == llama_agent_sdk::stop_reason::USER_CANCELLED ? "User cancelled"
                                                                   : res.final_response;
    return tr;
  }

private:
  llama_agent_sdk::http_agent_session &parent_;
  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<sdk_subagent_task>> tasks_;

  static std::string generate_task_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 35);

    const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string id = "task-";
    for (int i = 0; i < 8; ++i) {
      id += charset[dis(gen)];
    }
    return id;
  }

  std::string build_system_prompt(const subagent_type_config &cfg) const {
    std::ostringstream prompt;
    const auto &base = parent_.messages().at(0).value("content", std::string());
    if (!base.empty()) {
      prompt << base;
      prompt << "# Subagent Mode: " << cfg.name << "\n\n";
    } else {
      prompt << "You are a specialized " << cfg.name << " subagent.\n\n";
    }

    prompt << cfg.description << "\n\n";
    prompt << "## Tools Availavle in This Mode\n\n";
    prompt << "You have access to: ";
    bool first = true;
    for (const auto &t : cfg.allowed_tools) {
      if (!first) {
        prompt << ", ";
      }
      prompt << t;
      first = false;
    }
    prompt << "\n\n";

    if (cfg.name == "explore") {
      prompt << R"(# Guidelines

You are in READ-ONLY mode. Youre task is to explore and understand the codebase.

- Use `glob` to find files matching patterns
- Use `read` to examine file contents
- Use `bash` ONLY for read-only commands: ls ,cat ,head, tail, grep, find, git status, git log, git diff
- DO NOT modify any files
- DO NOT run destructive commands

Be thorough but efficient. Report what you find clearly.
)";
    } else if (cfg.name == "plan") {
      prompt << R"(# Guidelines

You are a planning agent. Your task is to design an implementation approach.

- Use `glob` and `read` to understand existing code structure
- Identify patterns and conventions in the codebase
- Consider edge case and potential issues
- Provide a clear, actionable plan

Output a structured plan with:
1. Overview of the approach
2. Files to modify/create
3. Step-by-step implementation details
4. Potential risks or considerations
)";
    } else if (cfg.name == "bash") {
      prompt << R"(# Guidelines

You are a command execution agent. Run shell commands to complete the task.

- Execute commands carefully
- Check command output for errors
- Report results clearly
)";
    } else {
      prompt << R"(# Guidelines

You are a general-purpose task agent. Complete the assigned task efficiently.

- Read files before modifying them
- Make targeted edits rathe than full rewrites
- Tests changes when possible
- Report what you accomlished
)";
    }

    return prompt.str();
  }
};

static std::mutex g_runner_mutex;
static std::map<std::string, std::shared_ptr<sdk_subagent_runner>> g_runners;

static std::string runner_key(const tool_context &ctx) {
  std::ostringstream ss;
  ss << "runner_" << ctx.server_ctx_ptr;
  return ss.str();
}

static sdk_subagent_runner &get_runner(const tool_context &ctx) {
  std::lock_guard<std::mutex> lock(g_runner_mutex);
  std::string key = runner_key(ctx);
  auto it = g_runners.find(key);
  if (it == g_runners.end()) {
    auto *session =
        static_cast<llama_agent_sdk::http_agent_session *>(ctx.server_ctx_ptr);
    g_runners[key] = std::make_shared<sdk_subagent_runner>(*session);
  }
  return *g_runners[key];
}

static tool_result task_execute(const json &args, const tool_context &ctx) {
  if (!ctx.server_ctx_ptr) {
    return {false, "", "Internal error: session context not initialized"};
  }

  std::string type_str = args.value("subagent_type", "general");
  std::string prompt = args.value("prompt", "");
  bool run_in_background = args.value("run_in_background", false);
  std::string resume_id = args.value("resume", "");

  subagent_type type;
  try {
    type = parse_subagent_type(type_str);
  } catch (const std::exception &e) {
    return {false,
            "",
            std::string("Invalid subagent type: ") + e.what() +
                ". Valid types: explore, plan, general, bash"};
  }

  auto &runner = get_runner(ctx);

  if (!resume_id.empty()) {
    if (runner.is_complete(resume_id)) {
      return runner.get_result(resume_id);
    }
    return {true,
            "Task " + resume_id +
                " is still running. Call task with resume=\"" + resume_id +
                "\" again later to get result.",
            ""};
  }

  if (prompt.empty()) {
    return {false, "", "The 'prompt' parameter is required for new tasks"};
  }

  if (run_in_background) {
    std::string task_id = runner.start_background(type, prompt);
    std::ostringstream output;
    output << "Started background task: " << task_id << "\n";
    output << "Type: " << subagent_type_name(type) << "\n\n";
    output << "To check status or get results, call task with resume=\"" << task_id
           << "\"";
    return {true, output.str(), ""};
  }

  tool_result r = runner.run(type, prompt);
  if (r.success) {
    return r;
  }
  if (!r.output.empty()) {
    return r;
  }
  return r;
}

static tool_def task_tool = {
    "task",
    "Spawn a subagent to handle a complex task automatically. Use for "
    "parallel exploration, "
    "planning, or delegating multi-step operations. The subagent runs "
    "with restricted tools "
    "base on its type and returns results when complete.\n\n"
    "Type:\n"
    "- explore: Read-only codebase exploration (glob, read, limiited bash)\n"
    "- plan: Architecture and design planning (glob, read)\n"
    "- general: Multi-step task execution (all tools except task)\n"
    "- bash: Shell command execution only\n\n"
    "Background mode:\n"
    "- Set run_in_background=true to start the task without waiting\n"
    "- Returns a task_id that can be used with the resume parameter\n"
    "- Call again with resume=\"task_id\" to check status or get results",
    R"json({
        "type": "object",
        "properties" : {
            "subagent_type": {
                "type": "string",
                "enum": ["explore", "plan", "general", "bash"],
                "description": "Type of subagent to spawn. Each type has different tool access.",
                "default": "general"
            },
            "prompt": {
                "type": "string",
                "description": "The task description for the subagent to execute. Required for new tasks."
            },
            "run_in_background": {
                "type": "boolean",
                "description": "If true, start the task in background and return immediately with a task_id",
                "default": false
            },
            "resume": {
                "type": "string",
                "description": "Task ID to resume/check status. When provided, other parameters are ignored."
            }
    },
    "required": []
    })json",
    task_execute};

REGISTER_TOOL(task, task_tool);

} // namespace
