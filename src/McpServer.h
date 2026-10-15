#pragma once

// The MCP endpoint (Streamable HTTP, JSON responses): JSON-RPC requests on POST /mcp from
// 127.0.0.1 only, with the token as a bearer token. Tool calls run on the editor's main thread
// through the runner; everything else answers on the server's threads.

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace aiassist {

class McpServer {
public:
    // Runs the function on the editor's main thread and returns when it is done; throws when
    // the plugin is unloading.
    using MainThreadRunner = std::function<void(const std::function<void()>&)>;
    // The tools' work, on the main thread: (tool name, arguments as JSON text) -> result JSON
    // text.
    using ToolCaller = std::function<std::string(const std::string&, const std::string&)>;

    McpServer(MainThreadRunner runOnMainThread, ToolCaller callTool);
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Listens on 127.0.0.1 at the port or one of the nine after it. Returns false and fills
    // error when none is free.
    bool start(int port, const std::string& token, std::string* error);
    [[nodiscard]] int port() const { return m_port; }
    void stop();

private:
    std::string handle(const std::string& body, bool& onlyNotifications);

    MainThreadRunner m_runOnMainThread;
    ToolCaller m_callTool;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_thread;
    std::string m_token;
    int m_port = 0;
};

}  // namespace aiassist
