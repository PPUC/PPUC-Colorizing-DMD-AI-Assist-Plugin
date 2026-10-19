// The plugin's entry points: loaded by the ColorizingDMD editor from its plugin folder (plugin
// id "AIAssist", see plugin.cfg).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"

#include "ColorizingDMDPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/MsgPlugin.h"

#include "Editor.h"
#include "McpServer.h"
#include "Tools.h"

namespace {

constexpr int kDefaultPort = 8975;

const MsgPluginAPI* g_msgApi = nullptr;
uint32_t g_endpoint = 0;
const ColorizingDMDEditorAPI* g_editorApi = nullptr;
LoggingPluginAPI* g_logging = nullptr;
std::unique_ptr<aiassist::McpServer> g_server;
std::string g_token;

// Requests waiting for the main thread, and whether the plugin is unloading (see unload).
std::mutex g_jobsMutex;
int g_jobsInFlight = 0;
bool g_stopping = false;

void log(unsigned int level, const std::string& message)
{
    if (g_logging) {
        g_logging->Log("AIAssist", "", 0, level, message.c_str());
    }
}

struct Job {
    const std::function<void()>* work;
    std::exception_ptr error;
};

void MSGPIAPI runJob(void* userData)
{
    Job* job = static_cast<Job*>(userData);
    try {
        (*job->work)();
    } catch (...) {
        job->error = std::current_exception();
    }
}

// On a server thread: runs the work on the editor's main thread and waits for it.
void runOnMainThread(const std::function<void()>& work)
{
    {
        const std::lock_guard<std::mutex> lock(g_jobsMutex);
        if (g_stopping) {
            throw std::runtime_error("the plugin is unloading");
        }
        ++g_jobsInFlight;
    }
    Job job{&work, nullptr};
    g_msgApi->RunOnMainThread(g_endpoint, -1.0, &runJob, &job);
    {
        const std::lock_guard<std::mutex> lock(g_jobsMutex);
        --g_jobsInFlight;
    }
    if (job.error) {
        std::rethrow_exception(job.error);
    }
}

std::string newToken()
{
    std::random_device random;
    std::string token;
    for (int i = 0; i < 4; ++i) {
        char part[9];
        std::snprintf(part, sizeof(part), "%08x", random());
        token += part;
    }
    return token;
}

std::string setting(const char* key)
{
    const uint32_t size = g_editorApi->GetSetting(g_endpoint, key, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::string value(size, '\0');
    g_editorApi->GetSetting(g_endpoint, key, value.data(), size);
    value.resize(size - 1);
    return value;
}

std::string connectionText()
{
    const std::string url = "http://127.0.0.1:" + std::to_string(g_server->port()) + "/mcp";
    return "The AI assistant reaches this editor at\n" + url +
           "\n\nClaude Code, once (in a terminal):\n"
           "claude mcp add --transport http colorizingdmd " +
           url + " --header \"Authorization: Bearer " + g_token +
           "\"\n\nThe endpoint answers only on this computer and only with this token, "
           "which stays the same for the next starts. The editor must be running with a "
           "project open.";
}

void MSGPIAPI onConnectionEntry(void*)
{
    if (g_server) {
        g_editorApi->ShowMessage(g_endpoint, "AI Assist", connectionText().c_str());
    }
}

}  // namespace

MSGPI_EXPORT void MSGPIAPI AIAssistPluginLoad(const uint32_t endpointId, const MsgPluginAPI* api)
{
    g_msgApi = api;
    g_endpoint = endpointId;
    g_stopping = false;

    const unsigned int getLogging = api->GetMsgID(LOGPI_NAMESPACE, LOGPI_MSG_GET_API);
    api->BroadcastMsg(endpointId, getLogging, &g_logging);
    api->ReleaseMsgID(getLogging);

    const unsigned int getEditor = api->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_MSG_GET_API);
    api->BroadcastMsg(endpointId, getEditor, &g_editorApi);
    api->ReleaseMsgID(getEditor);
    if (!g_editorApi || g_editorApi->version < 1) {
        log(LPI_LVL_ERROR, "Not loaded by the ColorizingDMD editor: no editor API; nothing to do.");
        g_editorApi = nullptr;
        return;
    }

    g_token = setting("token");
    if (g_token.empty()) {
        g_token = newToken();
        g_editorApi->SetSetting(endpointId, "token", g_token.c_str());
    }
    int port = kDefaultPort;
    try {
        const std::string stored = setting("port");
        if (!stored.empty()) {
            port = std::stoi(stored);
        }
    } catch (const std::exception&) {
        port = kDefaultPort;
    }

    g_server = std::make_unique<aiassist::McpServer>(
        [](const std::string& name, const std::string& arguments) {
            const aiassist::Editor editor(g_editorApi, g_endpoint, &runOnMainThread);
            return aiassist::callTool(editor, name, nlohmann::json::parse(arguments)).dump();
        });
    std::string error;
    if (!g_server->start(port, g_token, &error)) {
        log(LPI_LVL_ERROR, "MCP endpoint not started: " + error);
        g_server.reset();
        return;
    }
    log(LPI_LVL_INFO, "MCP endpoint at http://127.0.0.1:" + std::to_string(g_server->port()) +
                          "/mcp (Plugins -> AI Assist: Connection... shows how to connect)");
    g_editorApi->AddMenuEntry(endpointId, "AI Assist: Connection...", &onConnectionEntry, nullptr);
}

MSGPI_EXPORT void MSGPIAPI AIAssistPluginUnload()
{
    if (g_editorApi) {
        g_editorApi->RemoveMenuEntries(g_endpoint);
    }
    if (g_server) {
        {
            const std::lock_guard<std::mutex> lock(g_jobsMutex);
            g_stopping = true;
        }
        // A request may wait for the main thread, this one: run what is queued until none
        // waits, so the server's threads can end.
        std::thread stopper([]() { g_server->stop(); });
        for (;;) {
            g_msgApi->FlushPendingCallbacks(g_endpoint);
            {
                const std::lock_guard<std::mutex> lock(g_jobsMutex);
                if (g_jobsInFlight == 0) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stopper.join();
        g_server.reset();
    }
    g_editorApi = nullptr;
    g_logging = nullptr;
    g_msgApi = nullptr;
}
