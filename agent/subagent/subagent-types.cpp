#include "subagent-types.h"

#include <map>
#include <stdexcept>

// ANSI color codes
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_GREEN "\x1b[32m"

static const std::map<subagent_type, subagent_type_config> SUBAGENT_CONFIGS = {
    {subagent_type::EXPLORE,
     {
         "explore",
         "Read-only exploration if codebase",
         "\xE2\x9A\xA1", // Lightning bolt icon (U+26A1)
         ANSI_CYAN,
         {"read", "glob", "bash"},
         // Allowed bash command prefixes for read-only exploration
         {"ls", "cat", "head", "tail", "grep", "find", "file", "wc",
          "git status", "git log", "git diff", "git branch", "git show", "tree",
          "which", "type", "pwd"},
         false, // can_write_files
         20     // max_iterations
     }},
    {subagent_type::PLAN,
     {
         "plan",
         "Architecture and design planning",
         "\xF0\x9F\x93\x90", // Notebook (U+1F4D0)
         ANSI_MAGENTA,
         {"read", "glob"},
         {},    // No bash allowed
         false, // can_write_files
         15     // max_iterations

     }},
    {subagent_type::GENERAL,
     {
         "general",
         "General-purpose task execution",
         "\xF0\x9F\x94\xA7", // Wrench (U+1F527)
         ANSI_YELLOW,
         {"read", "glob", "write", "edit", "bash"}, // All except task
         {},                                        // No bash allowed
         true,                                      // can_write_files
         30                                         // max_iterations
     }},
    {subagent_type::BASH,
     {
         "bash",
         "Shell Command execution",
         "\xF0\x9F\x96\xA5", // Shell icon(Desktop computer) (U+1F6A5)
         ANSI_GREEN,
         {"bash"},
         {},    // No bash allowed
         false, // can_write_files
         10     // max_iterations
     }},

};

const subagent_type_config& get_subagent_config(subagent_type type) {
  auto it = SUBAGENT_CONFIGS.find(type);
  if (it == SUBAGENT_CONFIGS.end()) {
    throw std::runtime_error("Unknown subagent type");
  }
  return it->second;
}

subagent_type parse_subagent_type(const std::string &str) {
  if (str == "explore") {
    return subagent_type::EXPLORE;
  } else if (str == "plan") {
    return subagent_type::PLAN;
  } else if (str == "general") {
    return subagent_type::GENERAL;
  } else if (str == "bash") {
    return subagent_type::BASH;
  } else {
    throw std::runtime_error("Unknown subagent type: " + str);
  }
}

const char* subagent_type_name(subagent_type type) {
  switch (type) {
    case subagent_type::EXPLORE:
      return "explore";
    case subagent_type::PLAN:
      return "plan";
    case subagent_type::GENERAL:
      return "general";
    case subagent_type::BASH:
      return "bash";
    default:
      return "unknown";
  }
}