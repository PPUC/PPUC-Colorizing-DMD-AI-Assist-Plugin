#pragma once

// The MCP tools: what the assistant can ask the editor. They run on the editor's main thread
// (McpServer takes care of that).

#include <string>

#include "nlohmann/json.hpp"

#include "Editor.h"

namespace aiassist {

// The tools' definitions, for tools/list.
[[nodiscard]] nlohmann::json toolList();

// Runs a tool; the result is an MCP CallToolResult (content, isError).
[[nodiscard]] nlohmann::json callTool(const Editor& editor,
                                      const std::string& name,
                                      const nlohmann::json& arguments);

// What the assistant is told when it connects (the initialize result's instructions).
[[nodiscard]] std::string serverInstructions();

}  // namespace aiassist
