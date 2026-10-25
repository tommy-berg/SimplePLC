#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "web/api_service.h"
#include "web/auth.h"
#include "web/fleet.h"

namespace simpleplc::web {

/**
 * Model Context Protocol server: lets AI agents set up PLCs, write their Structured Text
 * programs, define tags, read and write values and talk Modbus to any device.
 *
 * Transport-independent: handle() takes one JSON-RPC message and returns the
 * response (nothing for notifications). The web server exposes it as
 * Streamable HTTP on /mcp; `simpleplc mcp` exposes it on stdio.
 *
 * In a fleet, a tool call about one PLC goes to the device that runs it, so an
 * agent programs a whole network through one endpoint with the same tools.
 */
class McpServer {
public:
    explicit McpServer(ApiService& api, Fleet* fleet = nullptr);

    /// Answers one JSON-RPC message (nothing for notifications and client responses). Never throws:
    /// a malformed message gets a JSON-RPC error (-32600 invalid request, -32602 invalid params).
    std::optional<json> handle(const json& message, const Principal& principal);

    /// MCP protocol versions this server speaks (the MCP-Protocol-Version header).
    static bool supportsProtocol(std::string_view version);
    static std::string supportedProtocols();

    struct Tool {
        std::string name;
        std::string title;
        std::string description;
        json inputSchema;    ///< also checked: required, types, ranges; unknown arguments are refused
        bool readOnly = false;
        bool destructive = false;  ///< set from one table after all tools are added
        std::function<json(const json& args, const Principal& principal)> run;
        bool idempotent = false;   ///< likewise
    };

private:
    json initialize(const json& params) const;
    json listTools() const;
    json callTool(const json& params, const Principal& principal);
    /// Fleet: answers list_plcs for all devices and forwards calls about one PLC to its device.
    json callFleetTool(const Tool& tool, json args, const Principal& principal);
    /// Fleet: a JSON-RPC call to the device that runs `plc`; returns its result or throws RpcError.
    /// `who`: who asked, for the device's events (tool calls; reads need no author).
    json forwardToDevice(const std::string& plc, const std::string& method, const json& params, const Principal* who = nullptr) const;
    json plcTags(const std::string& plc) const;
    json listResources() const;
    json listResourceTemplates() const;
    json readResource(const json& params) const;
    json listPrompts() const;
    json getPrompt(const json& params) const;

    ApiService& api_;
    Fleet* fleet_;
    std::vector<Tool> tools_;
};

}  // namespace simpleplc::web
