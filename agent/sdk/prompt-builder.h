#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace llama_agent_sdk {

struct prompt_build_options {
  bool enable_skills = true;
  std::vector<std::string> extra_skills_paths;
  bool enable_agents_md = true;
  std::string config_dir_override;
};

struct prompt_build_result {
  std::string system_prompt;
  std::string skills_prompt_section;
  std::string agents_md_prompt_section;
  std::vector<std::string> skills_search_paths;
  size_t skills_count = 0;
  size_t agents_md_count = 0;
};

std::string default_config_dir();

prompt_build_result build_system_prompt(const std::string &base_prompt,
                                       const std::string &working_dir,
                                       const prompt_build_options &options);

} // namespace llama_agent_sdk

