#include "mcp-tool-wrapper.h"
#include "../tool-registry.h"
#include "mcp-client.h"
#include "mcp-server-manager.h"
#include <string>

void register_mcp_tools(mcp_server_manager &manager) {
  auto tools = manager.list_all_tools();

  // mcp_tool from mcp_client
  for (auto &[qualified_name, mcp_tool] : tools) {
    tool_def wrapper; // tool-registry.h
    wrapper.name = qualified_name;
    wrapper.description = mcp_tool.description;

    // Convert MCP input schema to JSON string
    if (mcp_tool.input_schema.is_object()) {
        wrapper.parameters = mcp_tool.input_schema.dump();
    } else {
        wrapper.parameters = R"({"type": "object", "properties": {}})";
    }
    // Capture manager pointer and tool name for execution
    // Note: Manager must outlive these tool registrations
    mcp_server_manager *mgt_ptr = &manager;
    std::string tool_name = qualified_name;

    wrapper.execute = [mgt_ptr,
                       tool_name](const json &args,
                                  const tool_context &ctx) -> tool_result {
      (void)ctx; // MCP tools don't use local context
      try {
        mcp_call_result result = mgt_ptr->call_tool(tool_name, args);

        // Convert MCP content to string output
        std::string output;
        for (const auto& item: result.content) {
          std::string type = item.value("type", "");

          if (type == "text") {
            if (!output.empty()) output += "\n";
            output += item.value("text", "");
          } else if (type == "image") {
            if (!output.empty())
              output += "\n";
            output += "[Image: " + item.value("mimeType", "unknown") + "]";
          } else if (type == "resource") {
            if (!output.empty())
              output += "\n";
            output += "[Resource: " + item.value("uri", "unknown") + "]";
          }
        }

        if (result.is_error) {
          return {false, "", output.empty() ? "MCP tool returned error" : output};
        }
        return {true, output, ""};
      } catch (const std::exception& e) {
        return {false, "", std::string("MCP error: ") + e.what()};
      }
    };
    tool_registry::instance().register_tool(wrapper);
  }
}
