#pragma once
#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "web/api_service.h"
#include "web/auth.h"
#include "web/fleet.h"
#include "web/mcp.h"

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace simpleplc::web {

/**
 * HTTP server for the web panel (embedded files), the REST API under /api/v1
 * and the MCP endpoint /mcp. Live data uses Server-Sent Events.
 *
 * With a fleet, the same API covers the PLCs of many devices: everything under
 * /api/v1/plcs/{id} goes to the device that runs that PLC.
 */
class WebServer {
public:
    WebServer(ApiService& api, Auth& auth, McpServer& mcp, Fleet* fleet = nullptr);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    /// Listens on `listen:port`; when the port is busy and `tryNext`, on the next free one (up to +10).
    std::expected<int, std::string> start(const std::string& listen, int port, bool tryNext);
    void stop();

    enum class Access { Public, Viewer, Admin };
    using Handler = std::function<void(const httplib::Request&, httplib::Response&, const Principal&)>;
    struct Route {
        std::string method;
        std::string path;     ///< cpp-httplib pattern, e.g. /api/v1/plcs/:id
        std::string summary;
        Access access;
        Handler handler;
    };

private:
    void addRoutes();
    void registerRoutes();
    /// Throws ApiError 429 while the client is blocked after too many wrong tokens.
    std::optional<Principal> authenticate(const httplib::Request& req, bool& viaCookie);
    /// For live streams: re-checks the request's credentials, so a revoked token, a deleted
    /// user or a changed password ends the stream.
    std::function<bool()> stillAuthorized(const httplib::Request& req);
    void serveFile(const httplib::Request& req, httplib::Response& res, std::string path);
    void streamPlc(const std::string& id, httplib::Response& res, std::function<bool()> authorized);
    void streamEvents(httplib::Response& res, uint64_t sinceSeq, std::function<bool()> authorized);
    void handleMcp(const httplib::Request& req, httplib::Response& res);
    /// Fleet: passes a request about one PLC (matched by `route`) to its device and the answer back.
    /// Relays a request about one PLC to its device, saying who asked (`who`, for the device's events).
    void proxyToDevice(const httplib::Request& req, httplib::Response& res, const std::string& route, const Principal& who);
    /// Fleet: pipes a PLC's live events from its device.
    void streamFromDevice(const httplib::Request& req, httplib::Response& res);
    nlohmann::json plcList() const;
    nlohmann::json openApi() const;

    ApiService& api_;
    Auth& auth_;
    McpServer& mcp_;
    Fleet* fleet_;
    std::unique_ptr<httplib::Server> server_;
    std::vector<Route> routes_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> streams_{0};
};

}  // namespace simpleplc::web
