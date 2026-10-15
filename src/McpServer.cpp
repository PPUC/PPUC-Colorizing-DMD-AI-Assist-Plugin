#include "McpServer.h"

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "Tools.h"

namespace aiassist {

using nlohmann::json;

namespace {

constexpr const char* kProtocolVersion = "2025-06-18";

json errorResponse(const json& id, int code, const std::string& message)
{
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json resultResponse(const json& id, json result)
{
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// Only pages of this computer may call (a web page elsewhere could otherwise reach the port
// through the browser); clients that are not browsers send no Origin.
bool originAllowed(const std::string& origin)
{
    return origin.empty() || origin.rfind("http://127.0.0.1", 0) == 0 ||
           origin.rfind("http://localhost", 0) == 0;
}

}  // namespace

McpServer::McpServer(ToolCaller callTool)
    : m_callTool(std::move(callTool))
{
}

McpServer::~McpServer()
{
    stop();
}

bool McpServer::start(int port, const std::string& token, std::string* error)
{
    m_token = token;
    m_server = std::make_unique<httplib::Server>();
    m_server->new_task_queue = []() { return new httplib::ThreadPool(4); };
    m_server->Post("/mcp", [this](const httplib::Request& request, httplib::Response& response) {
        if (!originAllowed(request.get_header_value("Origin"))) {
            response.status = 403;
            return;
        }
        if (request.get_header_value("Authorization") != "Bearer " + m_token) {
            response.status = 401;
            response.set_content(R"({"error":"missing or wrong token"})", "application/json");
            return;
        }
        bool onlyNotifications = false;
        const std::string answer = handle(request.body, onlyNotifications);
        if (onlyNotifications) {
            response.status = 202;
            return;
        }
        response.set_content(answer, "application/json");
    });
    // No server-sent events: the server answers each POST with JSON.
    m_server->Get("/mcp", [](const httplib::Request&, httplib::Response& response) {
        response.status = 405;
    });
    m_server->Delete("/mcp", [](const httplib::Request&, httplib::Response& response) {
        response.status = 200;
    });

    for (int candidate = port; candidate < port + 10; ++candidate) {
        if (m_server->bind_to_port("127.0.0.1", candidate)) {
            m_port = candidate;
            m_thread = std::thread([this]() { m_server->listen_after_bind(); });
            return true;
        }
    }
    if (error) {
        *error = "no free port from " + std::to_string(port) + " to " + std::to_string(port + 9);
    }
    m_server.reset();
    return false;
}

void McpServer::stop()
{
    if (m_server) {
        m_server->stop();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_server.reset();
    m_port = 0;
}

std::string McpServer::handle(const std::string& body, bool& onlyNotifications)
{
    json request;
    try {
        request = json::parse(body);
    } catch (const json::exception&) {
        onlyNotifications = false;
        return errorResponse(nullptr, -32700, "Parse error").dump();
    }
    const bool batch = request.is_array();
    const json messages = batch ? request : json::array({request});
    json responses = json::array();
    for (const json& message : messages) {
        if (!message.is_object() || !message.contains("method")) {
            continue;  // A response from the client, or nothing: no answer.
        }
        if (!message.contains("id")) {
            continue;  // A notification (notifications/initialized and the like).
        }
        const json id = message["id"];
        const std::string method = message["method"].get<std::string>();
        const json params = message.value("params", json::object());
        try {
            if (method == "initialize") {
                const std::string asked = params.value("protocolVersion", std::string());
                const bool known = asked == "2025-06-18" || asked == "2025-03-26" ||
                                   asked == "2024-11-05";
                responses.push_back(resultResponse(
                    id, {{"protocolVersion", known ? asked : std::string(kProtocolVersion)},
                         {"capabilities", {{"tools", json::object()}}},
                         {"serverInfo", {{"name", "colorizingdmd-ai-assist"}, {"version", "0.1.0"}}},
                         {"instructions", serverInstructions()}}));
            } else if (method == "ping") {
                responses.push_back(resultResponse(id, json::object()));
            } else if (method == "tools/list") {
                responses.push_back(resultResponse(id, {{"tools", toolList()}}));
            } else if (method == "tools/call") {
                const std::string name = params.value("name", std::string());
                const std::string arguments = params.value("arguments", json::object()).dump();
                responses.push_back(resultResponse(id, json::parse(m_callTool(name, arguments))));
            } else {
                responses.push_back(errorResponse(id, -32601, "Method not found: " + method));
            }
        } catch (const std::exception& error) {
            responses.push_back(errorResponse(id, -32603, error.what()));
        }
    }
    onlyNotifications = responses.empty();
    if (onlyNotifications) {
        return std::string();
    }
    return batch ? responses.dump() : responses.front().dump();
}

}  // namespace aiassist
