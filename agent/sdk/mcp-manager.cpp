#include "mcp-manager.h"

#ifndef _WIN32

#include "../mcp/mcp-server-manager.h"
#include "../mcp/mcp-tool-wrapper.h"

#include <mutex>

namespace llama_agent_sdk {

void ensure_mcp_tools(const std::string &working_dir) {
  static std::mutex mutex;
  static bool initialized = false;
  static mcp_server_manager mgr;

  std::lock_guard<std::mutex> lock(mutex);
  if (initialized) {
    return;
  }

  std::string wd = working_dir.empty() ? "." : working_dir;
  std::string cfg = find_mcp_config(wd);
  if (!cfg.empty()) {
    if (mgr.load_config(cfg)) {
      int started = mgr.start_servers();
      if (started > 0) {
        register_mcp_tools(mgr);
      }
    }
  }

  initialized = true;
}

} // namespace llama_agent_sdk

#else

namespace llama_agent_sdk {

void ensure_mcp_tools(const std::string &) {}

} // namespace llama_agent_sdk

#endif

