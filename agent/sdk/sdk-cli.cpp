#include "http-agent.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

static std::string get_arg(int argc, char **argv, const std::string &name,
                           const std::string &def = "") {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == name) {
      return argv[i + 1];
    }
  }
  return def;
}

static bool has_flag(int argc, char **argv, const std::string &name) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == name) {
      return true;
    }
  }
  return false;
}

static std::string read_stdin_all() {
  std::ostringstream ss;
  ss << std::cin.rdbuf();
  return ss.str();
}

static std::string default_system_prompt() {
  return R"(You are llama-agent, a powerful local AI coding assistant running on llama.cpp.

You help users with software engineering tasks by reading files, writing code, running commands, and navigating codebases. You run entirely on the user's machine - no data leaves their system.

# Tools

You have access to the following tools:

- **bash**: Execute shell commands. Use for git, build commands, running tests, etc.
- **read**: Read file contents with line numbers. Always read files before editing them.
- **write**: Create new files or overwrite existing ones.
- **edit**: Make targeted edits using search/replace. The old_string must match exactly.
- **glob**: Find files matching a pattern. Use to explore project structure.
- **task**: Spawn a subagent with restricted tools for complex tasks.

# Guidelines

- Give short, clear responses. No filler.
- Read before you write. Prefer targeted edits.
- Avoid destructive shell commands.
)";
}

static void usage() {
  std::cerr << "Usage:\n";
  std::cerr << "  llama-agent-sdk --url http://localhost:8080 --model MODEL [--prompt \"...\"] [--working-dir .] [--yolo] [--no-stream] [--no-skills] [--no-agents-md] [--no-mcp]\n";
}

int main(int argc, char **argv) {
  std::string url = get_arg(argc, argv, "--url");
  std::string model = get_arg(argc, argv, "--model");
  std::string prompt = get_arg(argc, argv, "--prompt");
  std::string working_dir = get_arg(argc, argv, "--working-dir", ".");
  bool yolo = has_flag(argc, argv, "--yolo");
  bool no_stream = has_flag(argc, argv, "--no-stream");
  bool no_skills = has_flag(argc, argv, "--no-skills");
  bool no_agents_md = has_flag(argc, argv, "--no-agents-md");
  bool no_mcp = has_flag(argc, argv, "--no-mcp");

  if (url.empty() || model.empty()) {
    usage();
    return 2;
  }

  if (prompt.empty()) {
    prompt = read_stdin_all();
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r')) {
      prompt.pop_back();
    }
  }

  if (prompt.empty()) {
    usage();
    return 2;
  }

  llama_agent_sdk::http_server_config server;
  server.base_url = url;

  llama_agent_sdk::http_agent_config cfg;
  cfg.model = model;
  cfg.working_dir = working_dir;
  cfg.yolo_mode = yolo;
  cfg.system_prompt = default_system_prompt();
  cfg.enable_skills = !no_skills;
  cfg.enable_agents_md = !no_agents_md;
  cfg.enable_mcp = !no_mcp;

  llama_agent_sdk::http_agent_session session(server, cfg);

  auto on_event = [&](const llama_agent_sdk::event &ev) {
    using llama_agent_sdk::event_type;
    if (ev.type == event_type::TEXT_DELTA) {
      std::cout << ev.data.value("content", "") << std::flush;
      return;
    }
    if (ev.type == event_type::REASONING_DELTA) {
      return;
    }
    if (ev.type == event_type::PERMISSION_REQUIRED) {
      std::string rid = ev.data.value("required_id", "");
      std::string tool = ev.data.value("tool", "");
      std::string details = ev.data.value("details", "");
      bool dangerous = ev.data.value("dangerous", false);

      std::cerr << "\nPermission required for " << tool;
      if (dangerous) {
        std::cerr << " (dangerous)";
      }
      std::cerr << "\n" << details << "\n";
      std::cerr << "Allow? [y] once, [s] session, [n] deny: " << std::flush;

      std::string ans;
      std::getline(std::cin, ans);
      if (ans.empty()) {
        ans = "n";
      }
      if (ans == "y" || ans == "Y") {
        session.respond_permission(rid, true, permission_scope::ONCE);
      } else if (ans == "s" || ans == "S") {
        session.respond_permission(rid, true, permission_scope::SESSION);
      } else {
        session.respond_permission(rid, false, permission_scope::ONCE);
      }
      return;
    }
    if (ev.type == event_type::ERROR) {
      std::cerr << "\nERROR: " << ev.data.value("message", "") << "\n";
      return;
    }
  };

  llama_agent_sdk::run_result r;
  if (no_stream) {
    r = session.run(prompt);
    std::cout << r.final_response << "\n";
  } else {
    r = session.run_streaming(prompt, on_event);
    std::cout << "\n";
  }

  return r.reason == llama_agent_sdk::stop_reason::COMPLETED ? 0 : 1;
}
