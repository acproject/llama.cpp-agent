#include "prompt-builder.h"

#include "../agents-md/agents-md-manager.h"
#include "../skills/skills-manager.h"

#include <cstdlib>

namespace llama_agent_sdk {

std::string default_config_dir() {
#ifdef _WIN32
  const char *appdata = std::getenv("APPDATA");
  if (appdata) {
    return std::string(appdata) + "\\llama-agent";
  }
  return "";
#else
  const char *home = std::getenv("HOME");
  if (home) {
    return std::string(home) + "/.llama-agent";
  }
  return "";
#endif
}

static void ensure_trailing_newline(std::string &s) {
  if (!s.empty() && s.back() != '\n') {
    s.push_back('\n');
  }
}

prompt_build_result build_system_prompt(const std::string &base_prompt,
                                       const std::string &working_dir,
                                       const prompt_build_options &options) {
  prompt_build_result out;
  out.system_prompt = base_prompt;

  std::string config_dir = options.config_dir_override.empty()
                               ? default_config_dir()
                               : options.config_dir_override;

  std::string wd = working_dir.empty() ? "." : working_dir;

  if (options.enable_skills) {
    skills_manager mgr;
    std::vector<std::string> paths;
    paths.push_back(wd + "/.llama-agent/skills");
    if (!config_dir.empty()) {
      paths.push_back(config_dir + "/skills");
    }
    for (const auto &p : options.extra_skills_paths) {
      paths.push_back(p);
    }
    out.skills_search_paths = paths;
    mgr.discover(paths);
    out.skills_count = mgr.get_skills().size();
    out.skills_prompt_section = mgr.generate_prompt_section();
    if (!out.skills_prompt_section.empty()) {
      ensure_trailing_newline(out.system_prompt);
      out.system_prompt += "\n" + out.skills_prompt_section + "\n";
    }
  }

  if (options.enable_agents_md) {
    agents_md_manager mgr;
    mgr.discover(wd, config_dir);
    out.agents_md_count = mgr.get_files().size();
    out.agents_md_prompt_section = mgr.generate_prompt_section();
    if (!out.agents_md_prompt_section.empty()) {
      ensure_trailing_newline(out.system_prompt);
      out.system_prompt += "\n" + out.agents_md_prompt_section + "\n";
    }
  }

  return out;
}

} // namespace llama_agent_sdk

