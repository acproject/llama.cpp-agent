#pragma once

#include <optional>
#include <string>
#include <vector>

// Represents a disconverd AGENTS.md file
struct agents_md_file {
  std::string path;          // Absolute path to the file
  std::string content;       // Raw markdown content
  std::string relative_path; // Path relative to git root (for display)
  int depth;                 // Distance from working dir (0 = working dir)
};

// Manages AGENTS.md discovery and prompt generation
// Implements agents.md specification (https://agents.md/)
class agents_md_manager {
public:
  // Discover AGENTS.md files starting from working_dir up to git root
  // Optionally includes global AGENTS.md from config_dir (lowest precedence)
  // Returns number of files discovered
  int discover(const std::string &working_dir);
  int discover(const std::string &working_dir, const std::string &config_dir);

  // Get all discovered files (ordered by depth, closest first)
  const std::vector<agents_md_file> &get_files() const { return files_; }

  // Generate XML section for system prompt injection
  // Returns empty string if no files discovered
  std::string generate_prompt_section() const;

private:
  std::vector<agents_md_file> files_; // Discovered files (ordered by depth)

  // Find git root from a starting directory
  // Returns empty string if not in a git repository
  static std::string find_git_root(const std::string &start_dir);

  // Read file content safely
  // Returns nullopt if file cannot be read or is binary
  static std::optional<std::string> read_file(const std::string &path);

  // Escape special XML characters in attribute values
  static std::string escape_xml_attr(const std::string &str);
};
