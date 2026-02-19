#include "tool-registry.h"
#include "chat.h"
#include <string>
#include <vector>

tool_registry &tool_registry::instance() {
  static tool_registry instance;
  return instance;
}

void tool_registry::register_tool(const tool_def &tool) {
  tools_[tool.name] = tool;
}

const tool_def *tool_registry::get_tool(const std::string &name) const {
  auto it = tools_.find(name);
  if (it != tools_.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<const tool_def*> tool_registry::get_all_tools() const {
  std::vector<const tool_def*> all_tools;
  for (const auto &[name, tool] : tools_) {
    all_tools.push_back(&tool);
  }
  return all_tools;
}

std::vector<common_chat_tool> tool_registry::to_chat_tools() const {
  std::vector<common_chat_tool> result;
  for (const auto & [name, tool] : tools_) {
    result.push_back(tool.to_chat_tool());
  }
  return result;
}

std::vector<common_chat_tool> tool_registry::to_chat_tools_filtered(
    const std::set<std::string> &allowed_tools) const {
  std::vector<common_chat_tool> result;
  for (const auto &[name, tool] : tools_) {
    if (allowed_tools.count(name)) {
      result.push_back(tool.to_chat_tool());
    }
  }
  return result;
}
tool_result tool_registry::execute(const std::string &name, const json &args,
                       const tool_context &ctx) const {
  const tool_def *tool = get_tool(name);
  if (!tool) {
    return {false, "", "Unknown tool: " + name};
  }
  try {
    return tool->execute(args, ctx);
  } catch (const std::exception &e) {
    return {false, "", std::string("Tool execution error: ") + e.what()};
  }
}

tool_result tool_registry::execute_filtered(
    const std::string &name, const json &args, const tool_context &ctx,
    const std::set<std::string> &bash_patterns) const {
  if (name == "bash" && !bash_patterns.empty()) {
    std::string cmd = args.value("command", "");

    bool allowed = false;
    for (const auto &pattern : bash_patterns) {
      // Check if command starts with the pattern or contains it after a
      // space/pipe/etc
      if (cmd.find(pattern) == 0 ||
          cmd.find(" " + pattern) != std::string::npos ||
          cmd.find("|" + pattern) != std::string::npos ||
          cmd.find("&" + pattern) != std::string::npos) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      return {false, "", "Command not allowed in read-only mode: " + cmd};
    }
  }
  return execute(name, args, ctx);
}
