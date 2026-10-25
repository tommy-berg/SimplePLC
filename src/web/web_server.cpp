#include "web/web_server.h"

#include <charconv>
#include <climits>
#include <format>
#include <map>
#include <regex>

#include <httplib.h>

#include "app/embedded.h"
#include "app/log.h"
#include "app/version.h"

namespace simpleplc::web {
namespace {

using httplib::Request;
using httplib::Response;

constexpr int kMaxStreams = 32;
/// How often a live stream re-checks its credentials.
constexpr auto kReauthorize = std::chrono::seconds(2);
constexpr const char* kSessionCookie = "simpleplc_session";

void sendJson(Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(dumpJson(body), "application/json");
}

void sendError(Response& res, const ApiError& e) {
    json error = {{"code", e.code}, {"message", e.what()}};
    if (!e.details.is_null()) error["details"] = e.details;
    sendJson(res, e.status, {{"error", error}});
}

json body(const Request& req) {
    if (req.body.empty()) return json::object();
    json j = parseJson(req.body);
    if (j.is_discarded()) {
        throw ApiError(400, "bad_request", std::format("the request body is not valid JSON (or nested deeper than {})", kMaxJsonDepth));
    }
    return j;
}

/// Program source from a request: plain text, or JSON {"source": "..."}.
std::string sourceFrom(const Request& req) {
    // A form (curl -d or -F) is not the program: its body is field=value pairs or MIME parts, and curl -d
    // also drops the line ends. Saving it would replace a working program with something else
    std::string type = req.get_header_value("Content-Type");
    for (const char* form : {"application/x-www-form-urlencoded", "multipart/"}) {
        if (type.starts_with(form)) {
            throw ApiError(415, "unsupported_media_type",
                           std::format("a program is sent as text/plain or as JSON {{\"source\": \"...\"}}, not as {} "
                                       "(with curl: --data-binary @main.st -H 'Content-Type: text/plain')",
                                       type.substr(0, type.find(';'))));
        }
    }
    if (type.starts_with("application/json")) {
        json j = body(req);
        if (!j.is_object() || !j.contains("source") || !j["source"].is_string()) {
            throw ApiError(400, "bad_request", "send the Structured Text as text/plain or as {\"source\": \"...\"}");
        }
        return j["source"].get<std::string>();
    }
    return req.body;
}

/// If-Match: "<sha256>" of a program save; none for a missing header or "*".
std::optional<std::string> ifMatch(const Request& req) {
    std::string value = req.get_header_value("If-Match");
    if (value.starts_with("W/")) value = value.substr(2);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    if (value.empty() || value == "*") return std::nullopt;
    return value;
}

std::string cookieValue(const Request& req, const std::string& name) {
    std::string cookies = req.get_header_value("Cookie");
    size_t pos = 0;
    while (pos < cookies.size()) {
        size_t end = cookies.find(';', pos);
        if (end == std::string::npos) end = cookies.size();
        std::string part = cookies.substr(pos, end - pos);
        size_t start = part.find_first_not_of(' ');
        if (start != std::string::npos) part = part.substr(start);
        if (part.starts_with(name + "=")) return part.substr(name.size() + 1);
        pos = end + 1;
    }
    return {};
}

void setSessionCookie(Response& res, const std::string& session, bool clear = false) {
    res.set_header("Set-Cookie", clear ? std::format("{}=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0", kSessionCookie)
                                       : std::format("{}={}; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800",
                                                     kSessionCookie, session));
}

std::string param(const Request& req, const char* name, std::string fallback = {}) {
    return req.has_param(name) ? req.get_param_value(name) : fallback;
}

int intParam(const Request& req, const char* name, int fallback, int min = INT_MIN, int max = INT_MAX) {
    if (!req.has_param(name)) return fallback;
    std::string text = req.get_param_value(name);
    int value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || text.empty() || value < min || value > max) {
        throw ApiError(400, "bad_request", min == INT_MIN ? std::format("query parameter '{}' must be a whole number", name)
                                                          : std::format("query parameter '{}' must be a whole number from {} to {}", name, min, max));
    }
    return value;
}

bool boolParam(const Request& req, const char* name, bool fallback) {
    if (!req.has_param(name)) return fallback;
    std::string v = req.get_param_value(name);
    return v == "1" || v == "true" || v == "yes";
}

/// The bearer token of an Authorization header; the scheme's case does not matter (RFC 9110 11.1).
/// A name from the on-behalf header, fit for an event: printable, at most 64 bytes, never empty.
std::string onBehalfName(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (out.size() >= 64) break;
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) out += c;
    }
    return out.empty() ? std::string("?") : out;
}

std::optional<std::string> bearerToken(const Request& req) {
    std::string value = req.get_header_value("Authorization");
    if (value.size() < 7) return std::nullopt;
    std::string scheme = value.substr(0, 7);
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "bearer ") return std::nullopt;
    size_t start = value.find_first_not_of(' ', 7);
    return start == std::string::npos ? std::string() : value.substr(start);
}

/// A PLC answer with its configuration's version also as the ETag, for If-Match.
json withVersion(Response& res, json plc) {
    if (plc.is_object() && plc.contains("config_version") && plc["config_version"].is_number_unsigned()) {
        res.set_header("ETag", std::format("\"{}\"", plc["config_version"].get<uint64_t>()));
    }
    return plc;
}

/// Conditions of a configuration change: If-Match: "<config_version>" (from GET .../plcs/:id), ?force=1, ?create=1.
ChangeOptions changeOptions(const Request& req) {
    ChangeOptions options;
    options.force = boolParam(req, "force", false);    // apply a tag change that stops a running program
    options.create = boolParam(req, "create", false);  // a task may name a new program (empty template)
    if (!req.has_header("If-Match")) return options;
    std::string value = req.get_header_value("If-Match");
    if (value == "*") return options;  // any current version
    std::string_view v = value;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    uint64_t version = 0;
    auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), version);
    if (ec != std::errc() || end != v.data() + v.size() || v.empty()) {
        throw ApiError(400, "bad_request", std::format("If-Match must be the configuration's version from GET /api/v1/plcs/<id> "
                                                       "(config_version), e.g. If-Match: \"12\"; got {}", value));
    }
    options.ifVersion = version;
    return options;
}

/// GET /plcs/:id/events: the list of events instead of the live stream, asked for with ?limit= or with an
/// Accept that names JSON but not event streams (a browser's EventSource sends Accept: text/event-stream).
bool wantsEventList(const Request& req) {
    std::string accept = req.get_header_value("Accept");
    if (accept.find("text/event-stream") != std::string::npos) return false;
    return req.has_param("limit") || accept.find("application/json") != std::string::npos;
}

/// A fleet operation's answer, or its failure as an API error.
json fleetCall(std::expected<json, Fleet::Failure> result) {
    if (!result) throw ApiError(result.error().status, result.error().code, result.error().message);
    return *result;
}

std::string contentType(std::string_view path) {
    static const std::map<std::string, std::string, std::less<>> types = {
        {".html", "text/html; charset=utf-8"},  {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},    {".svg", "image/svg+xml"},
        {".png", "image/png"},                  {".ico", "image/x-icon"},
        {".json", "application/json"},          {".md", "text/markdown; charset=utf-8"},
        {".woff2", "font/woff2"},               {".txt", "text/plain; charset=utf-8"}};
    size_t dot = path.rfind('.');
    if (dot != std::string_view::npos) {
        if (auto it = types.find(path.substr(dot)); it != types.end()) return it->second;
    }
    return "application/octet-stream";
}

std::string sseEvent(const char* name, const json& data) {
    return std::format("event: {}\ndata: {}\n\n", name, dumpJson(data));
}

/// Sleeps up to `ms` in short steps so shutdown is not delayed.
void nap(const std::atomic<bool>& stopping, int ms) {
    for (int waited = 0; waited < ms && !stopping; waited += 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace

WebServer::WebServer(ApiService& api, Auth& auth, McpServer& mcp, Fleet* fleet)
    : api_(api), auth_(auth), mcp_(mcp), fleet_(fleet), server_(std::make_unique<httplib::Server>()) {
    addRoutes();
    registerRoutes();
}

json WebServer::plcList() const { return fleet_ ? fleet_->plcs() : api_.listPlcs(); }

void WebServer::proxyToDevice(const Request& req, Response& res, const std::string& route, const Principal& who) {
    std::string id = req.path_params.at("id");
    auto url = fleet_->deviceFor(id);
    if (!url) throw ApiError(url.error().status, url.error().code, url.error().message);
    // A rename through the fleet must not give two devices the same id: the fleet could not tell them apart
    if ((route == "/api/v1/plcs/:id" && req.method == "PATCH") || route == "/api/v1/plcs/:id/setup") {
        json changes = parseJson(req.body);
        if (changes.is_object() && changes.contains("id") && changes["id"].is_string() && changes["id"] != id) {
            std::string next = changes["id"].get<std::string>();
            if (auto other = fleet_->otherDeviceWith(next, *url)) {
                throw ApiError(409, "conflict", std::format("PLC id '{}' is already used by {} in this fleet", next, *other));
            }
        }
    }
    std::string target = req.target;
    // The event list (not the stream): the device tells them apart by ?limit or Accept, and only the query reaches it
    if (route == "/api/v1/plcs/:id/events" && !req.has_param("limit")) target += (target.find('?') == std::string::npos ? "?" : "&") + std::string("limit=50");
    std::map<std::string, std::string> headers;
    if (req.has_header("If-Match")) headers["If-Match"] = req.get_header_value("If-Match");
    headers[kOnBehalfHeader] = who.onBehalfOf.empty() ? who.name : std::format("{} via {}", who.onBehalfOf, who.name);
    auto reply = fleet_->send(*url, req.method, target, req.body, req.get_header_value("Content-Type"), headers);
    if (!reply) throw ApiError(reply.error().status, reply.error().code, std::format("PLC '{}': {}", id, reply.error().message));
    // The device refusing the fleet's token is the fleet's problem, not the user's session (the panel would sign out)
    if (reply->status == 401 || reply->status == 403 || reply->status == 429) {
        throw ApiError(502, "device_refused", std::format("the device of PLC '{}' ({}) does not accept the fleet's token "
                                                          "(set SIMPLEPLC_DEVICE_TOKEN to its admin SIMPLEPLC_TOKEN)", id, *url));
    }
    // Only what devices send: JSON, or text for program sources. Anything else (HTML from a device that is
    // not what it seems) must not reach the browser on the fleet's origin.
    const std::string& type = reply->contentType;
    if (!type.starts_with("application/json") && !type.starts_with("text/plain")) {
        throw ApiError(502, "device_bad_answer", std::format("the device of PLC '{}' answered '{}', not JSON or text", id, type));
    }
    if (req.method != "GET" && reply->status < 300) fleet_->refresh(*url);  // e.g. a rename: the fleet knows at once
    if (!reply->etag.empty()) res.set_header("ETag", reply->etag);
    res.status = reply->status;
    res.set_content(reply->body, type);
}

void WebServer::streamFromDevice(const Request& req, Response& res) {
    std::string id = req.path_params.at("id");
    auto url = fleet_->deviceFor(id);
    if (!url) throw ApiError(url.error().status, url.error().code, url.error().message);
    // An offline device gets an error the panel can see, not an empty stream it reconnects to forever
    if (auto ready = fleet_->available(*url); !ready) {
        throw ApiError(ready.error().status, ready.error().code, std::format("PLC '{}': {}", id, ready.error().message));
    }
    if (streams_ >= kMaxStreams) throw ApiError(503, "busy", "too many live connections; close other panel tabs");
    streams_++;
    auto authorized = stillAuthorized(req);
    std::string target = req.target;
    std::string device = *url;
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, device, target, id, authorized](size_t, httplib::DataSink& sink) {
            auto lastAuth = std::chrono::steady_clock::now();
            auto streamed = fleet_->stream(device, target, [&](std::string_view chunk) {
                if (stopping_) return false;
                if (std::chrono::steady_clock::now() - lastAuth > kReauthorize) {
                    if (!authorized()) return false;
                    lastAuth = std::chrono::steady_clock::now();
                }
                return sink.write(chunk.data(), chunk.size());
            });
            // The device no longer has this PLC (renamed, set up anew): say so, as a device's own stream does.
            // A device that went offline or fell silent: say so too, so the panel stops showing its values as live.
            if (!streamed && streamed.error().status == 404) {
                std::string gone = sseEvent("gone", {{"id", id}});
                sink.write(gone.data(), gone.size());
            } else if (!streamed) {
                std::string offline = sseEvent("offline", {{"id", id}, {"code", streamed.error().code},
                                                           {"message", std::format("PLC '{}': {}", id, streamed.error().message)}});
                sink.write(offline.data(), offline.size());
            }
            sink.done();
            return true;
        },
        [this](bool) { streams_--; });
}

WebServer::~WebServer() { stop(); }

std::expected<int, std::string> WebServer::start(const std::string& listen, int port, bool tryNext) {
    int last = tryNext ? port + 10 : port;
    for (int p = port; p <= last; p++) {
        if (server_->bind_to_port(listen, p)) {
            thread_ = std::thread([this] { server_->listen_after_bind(); });
            server_->wait_until_ready();
            api_.setWebEndpoint(listen, p);
            return p;
        }
        // httplib marks a server whose bind failed as finished: try the next port with a new one
        server_ = std::make_unique<httplib::Server>();
        registerRoutes();
    }
    return std::unexpected(tryNext ? std::format("cannot listen on {} ports {}-{}", listen, port, last)
                                   : std::format("cannot listen on {}:{}", listen, port));
}

void WebServer::stop() {
    stopping_ = true;
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
}

std::optional<Principal> WebServer::authenticate(const Request& req, bool& viaCookie) {
    viaCookie = false;
    if (auto bearer = bearerToken(req)) {
        auto who = auth_.token(*bearer);
        // Wrong tokens are limited per address. A correct token nobody could have guessed still works from
        // a waiting address: one misconfigured client must not lock out the others behind it (NAT, a fleet)
        if (auto wait = auth_.blockedFor(Attempt::Token, req.remote_addr); wait && !(who && auth_.unguessable(*bearer))) {
            throw ApiError(429, "too_many_attempts", std::format("too many wrong tokens from this address; try again in {} seconds", wait));
        }
        if (!who) auth_.recordFailure(Attempt::Token, req.remote_addr);
        // A fleet says who asked it. Only an admin token may say so (it could do the change itself), and the
        // events keep the token's name too: "tommy via fleet:line2"
        if (who && who->role == Role::Admin && req.has_header(kOnBehalfHeader)) who->onBehalfOf = onBehalfName(req.get_header_value(kOnBehalfHeader));
        return who;
    }
    std::string session = cookieValue(req, kSessionCookie);
    if (!session.empty()) {
        viaCookie = true;
        return auth_.session(session);
    }
    return std::nullopt;
}

std::function<bool()> WebServer::stillAuthorized(const Request& req) {
    std::optional<std::string> bearer = bearerToken(req);
    std::string session = cookieValue(req, kSessionCookie);
    return [this, bearer, session] {
        if (bearer) return auth_.token(*bearer).has_value();
        return auth_.session(session).has_value();
    };
}

void WebServer::registerRoutes() {
    // httplib's default sets SO_REUSEPORT, which let a second process bind the same port and
    // take over the requests instead of falling back to the next port
    server_->set_socket_options([](socket_t sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    server_->new_task_queue = [] { return new httplib::ThreadPool(8, 64); };
    // Above the 1 MB program limit, so an oversized program gets the store's clear refusal, not a bare 413
    server_->set_payload_max_length(2 * 1024 * 1024);
    server_->set_read_timeout(std::chrono::seconds(30));
    server_->set_write_timeout(std::chrono::seconds(10));

    server_->set_post_routing_handler([](const Request& req, Response& res) {
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Referrer-Policy", "no-referrer");
        // API answers are data: opened directly in a browser, nothing in them may run
        if (req.path.starts_with("/api/") || req.path == "/mcp") {
            res.set_header("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'; sandbox");
        }
    });

    for (const Route& route : routes_) {
        auto handler = [this, &route](const Request& req, Response& res) {
            try {
                Principal principal{"anonymous", Role::Viewer, false, {}, {}};
                if (route.access != Access::Public) {
                    bool viaCookie = false;
                    auto who = authenticate(req, viaCookie);
                    if (!who) {
                        res.set_header("WWW-Authenticate", "Bearer");
                        throw ApiError(401, "unauthorized", "sign in, or send an API token as 'Authorization: Bearer <token>'");
                    }
                    // Cookies are sent by browsers on cross-site requests; require a header they cannot add
                    if (viaCookie && req.method != "GET" && req.get_header_value("X-Requested-With") != "SimplePLC") {
                        throw ApiError(403, "forbidden", "missing header X-Requested-With: SimplePLC");
                    }
                    if (route.access == Access::Admin && who->role != Role::Admin) {
                        throw ApiError(403, "forbidden", "this needs an administrator account or admin token");
                    }
                    principal = *who;
                }
                route.handler(req, res, principal);
            } catch (const ApiError& e) {
                sendError(res, e);
            } catch (const json::exception& e) {
                // A value of the wrong type in the request; say what, without the library's numbering
                std::string what = e.what();
                if (what.starts_with("[json.exception.")) what.erase(0, what.find("] ") + 2);
                sendError(res, ApiError(400, "bad_request", "a value in the request has the wrong type: " + what));
            } catch (const std::exception& e) {
                log::write(log::Level::Error, "web", "", std::format("{} {}: {}", req.method, req.path, e.what()));
                sendError(res, ApiError(500, "internal", e.what()));
            }
        };
        if (route.method == "GET") server_->Get(route.path, handler);
        else if (route.method == "POST") server_->Post(route.path, handler);
        else if (route.method == "PUT") server_->Put(route.path, handler);
        else if (route.method == "PATCH") server_->Patch(route.path, handler);
        else if (route.method == "DELETE") server_->Delete(route.path, handler);
    }

    server_->Post("/mcp", [this](const Request& req, Response& res) { handleMcp(req, res); });
    auto mcpOnlyPost = [](const Request&, Response& res) {
        res.status = 405;
        res.set_header("Allow", "POST");
    };
    server_->Get("/mcp", mcpOnlyPost);
    server_->Delete("/mcp", mcpOnlyPost);

    server_->Get("/", [this](const Request& req, Response& res) { serveFile(req, res, "index.html"); });
    server_->Get(R"(/assets/(.+))", [this](const Request& req, Response& res) { serveFile(req, res, req.matches[1].str()); });
    server_->Get("/favicon.ico", [this](const Request& req, Response& res) { serveFile(req, res, "icon.svg"); });

    server_->set_error_handler([](const Request& req, Response& res) {
        if (res.status == 404 && res.body.empty() && req.path.starts_with("/api/")) {
            sendError(res, ApiError(404, "not_found", std::format("no API route {} {}", req.method, req.path)));
        }
        // httplib refuses a body over the limit before any route runs: say so as JSON (MCP: as JSON-RPC)
        if (res.status == 413 && res.body.empty()) {
            std::string message = "the request body is larger than 2 MB; a program may have up to 1 MB";
            if (req.path == "/mcp") {
                sendJson(res, 413, {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32600}, {"message", message}}}});
            } else {
                sendError(res, ApiError(413, "too_large", message));
            }
        }
    });
}

void WebServer::serveFile(const Request& req, Response& res, std::string path) {
    const embedded::File* file = embedded::find("web/" + path);
    if (!file || path.find("..") != std::string::npos) {
        res.status = 404;
        return;
    }
    std::string etag = std::format("\"{}\"", kVersion);
    if (req.get_header_value("If-None-Match") == etag) {
        res.status = 304;
        return;
    }
    res.set_header("ETag", etag);
    res.set_header("Cache-Control", "no-cache");
    if (path == "index.html") {
        res.set_header("Content-Security-Policy",
                       "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                       "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    }
    res.set_content(reinterpret_cast<const char*>(file->data), file->size, contentType(path));
}

void WebServer::handleMcp(const Request& req, Response& res) {
    auto rpcError = [&](int status, int code, const std::string& message) {
        sendJson(res, status, {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", code}, {"message", message}}}});
    };
    try {
        // DNS-rebinding protection (MCP spec): a browser origin must match the host it talks to
        std::string origin = req.get_header_value("Origin");
        if (!origin.empty()) {
            std::string host = req.get_header_value("Host");
            if (!origin.ends_with("://" + host)) throw ApiError(403, "forbidden", "origin not allowed");
        }
        bool viaCookie = false;
        auto who = authenticate(req, viaCookie);
        if (!who || viaCookie) {
            res.set_header("WWW-Authenticate", "Bearer");
            throw ApiError(401, "unauthorized", "send an API token as 'Authorization: Bearer <token>' (create one under Users and tokens)");
        }
        // Streamable HTTP: a version the client says it speaks but the server does not is a 400 (MCP 2025-06-18)
        if (req.has_header("MCP-Protocol-Version")) {
            std::string version = req.get_header_value("MCP-Protocol-Version");
            if (!McpServer::supportsProtocol(version)) {
                rpcError(400, -32600, std::format("unsupported MCP-Protocol-Version '{}'; this server speaks {}", version,
                                                  McpServer::supportedProtocols()));
                return;
            }
        }
        json message = parseJson(req.body);
        if (message.is_discarded()) {
            rpcError(400, -32700, "Parse error: the body is not valid JSON (or nested deeper than " + std::to_string(kMaxJsonDepth) + ")");
            return;
        }
        if (message.is_array()) {
            // A JSON-RPC batch (protocol versions before 2025-06-18): every element on its own
            if (message.empty()) {
                rpcError(400, -32600, "Invalid Request: an empty batch");
                return;
            }
            json responses = json::array();
            for (const json& m : message) {
                if (auto r = mcp_.handle(m, *who)) responses.push_back(*r);
            }
            if (responses.empty()) {
                res.status = 202;
            } else {
                sendJson(res, 200, responses);
            }
            return;
        }
        auto response = mcp_.handle(message, *who);
        if (!response) {
            res.status = 202;
            return;
        }
        sendJson(res, 200, *response);
    } catch (const ApiError& e) {
        sendError(res, e);
    } catch (const std::exception& e) {
        // handle() answers every message itself; this is a last resort, never a bare 500
        log::write(log::Level::Error, "mcp", "", std::format("POST /mcp: {}", e.what()));
        rpcError(500, -32603, "Internal error");
    }
}

void WebServer::streamPlc(const std::string& id, Response& res, std::function<bool()> authorized) {
    if (streams_ >= kMaxStreams) throw ApiError(503, "busy", "too many live connections; close other panel tabs");
    api_.getPlc(id);  // 404 before the stream starts

    struct State {
        bool first = true;
        ApiService::LiveState live;  // what the client has: a poll builds JSON for the changes only
        std::chrono::steady_clock::time_point lastStatus{};
        std::chrono::steady_clock::time_point lastPing = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastAuth = std::chrono::steady_clock::now();
    };
    auto state = std::make_shared<State>();
    streams_++;
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, id, state, authorized](size_t, httplib::DataSink& sink) {
            if (stopping_) return false;
            std::string out;
            try {
                auto now = std::chrono::steady_clock::now();
                if (now - state->lastAuth > kReauthorize) {
                    if (!authorized()) return false;
                    state->lastAuth = now;
                }
                // Every 100 ms with 2000 tags, this must cost next to nothing while the PLC is quiet: the
                // image's versions decide whether anything is read at all
                if (auto values = api_.liveValues(id, state->live)) out += sseEvent("values", *values);
                if (state->first || now - state->lastStatus > std::chrono::seconds(1)) {
                    json summary = api_.getPlc(id);
                    out += sseEvent("status", summary);
                    state->lastStatus = now;
                }
                if (out.empty() && now - state->lastPing > std::chrono::seconds(15)) {
                    out = ": ping\n\n";
                }
                if (!out.empty()) state->lastPing = now;
                state->first = false;
            } catch (const ApiError&) {
                out = sseEvent("gone", {{"id", id}});
                sink.write(out.data(), out.size());
                return false;
            } catch (const std::exception&) {
                return false;
            }
            if (!out.empty() && !sink.write(out.data(), out.size())) return false;
            nap(stopping_, 100);
            return !stopping_;
        },
        [this](bool) { streams_--; });
}

void WebServer::streamEvents(Response& res, uint64_t sinceSeq, std::function<bool()> authorized) {
    if (streams_ >= kMaxStreams) throw ApiError(503, "busy", "too many live connections; close other panel tabs");
    struct State {
        uint64_t logSeq = 0;
        std::chrono::steady_clock::time_point lastPlcs{};
        std::chrono::steady_clock::time_point lastPing = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastAuth = std::chrono::steady_clock::now();
    };
    auto state = std::make_shared<State>();
    state->logSeq = sinceSeq;
    streams_++;
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, state, authorized](size_t, httplib::DataSink& sink) {
            if (stopping_) return false;
            std::string out;
            try {
                auto now = std::chrono::steady_clock::now();
                if (now - state->lastAuth > kReauthorize) {
                    if (!authorized()) return false;
                    state->lastAuth = now;
                }
                json logs = api_.logs(state->logSeq, "", "", 500);
                if (!logs["records"].empty()) out += sseEvent("logs", logs["records"]);
                state->logSeq = logs["last_seq"].get<uint64_t>();
                if (now - state->lastPlcs > std::chrono::seconds(2)) {
                    out += sseEvent("plcs", plcList());
                    state->lastPlcs = now;
                }
                if (out.empty() && now - state->lastPing > std::chrono::seconds(15)) out = ": ping\n\n";
                if (!out.empty()) state->lastPing = now;
            } catch (const std::exception&) {
                return false;
            }
            if (!out.empty() && !sink.write(out.data(), out.size())) return false;
            log::waitForNewer(state->logSeq, std::chrono::milliseconds(250));
            return !stopping_;
        },
        [this](bool) { streams_--; });
}

json WebServer::openApi() const {
    json paths = json::object();
    std::regex param(R"(:([A-Za-z_]+))");
    for (const Route& route : routes_) {
        std::string path = std::regex_replace(route.path, param, "{$1}");
        json op = {{"summary", route.summary}, {"responses", {{"200", {{"description", "OK"}}}}}};
        json parameters = json::array();
        for (std::sregex_iterator it(route.path.begin(), route.path.end(), param), end; it != end; ++it) {
            parameters.push_back({{"name", (*it)[1].str()}, {"in", "path"}, {"required", true}, {"schema", {{"type", "string"}}}});
        }
        if (!parameters.empty()) op["parameters"] = parameters;
        if (route.access == Access::Public) op["security"] = json::array();
        if (route.access == Access::Admin) op["description"] = "Requires an administrator.";
        std::string method = route.method;
        std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        paths[path][method] = op;
    }
    paths["/mcp"]["post"] = {{"summary", "Model Context Protocol endpoint (Streamable HTTP, JSON responses)"},
                             {"responses", {{"200", {{"description", "JSON-RPC response"}}}, {"202", {{"description", "Notification accepted"}}}}}};
    return {{"openapi", "3.1.0"},
            {"info", {{"title", "SimplePLC API"}, {"version", kVersion},
                      {"description", "Manage soft PLCs: programs, tags, values, users and tokens."}}},
            {"components", {{"securitySchemes", {{"bearer", {{"type", "http"}, {"scheme", "bearer"}}},
                                                 {"session", {{"type", "apiKey"}, {"in", "cookie"}, {"name", kSessionCookie}}}}}}},
            {"security", json::array({{{"bearer", json::array()}}, {{"session", json::array()}}})},
            {"paths", paths}};
}

void WebServer::addRoutes() {
    auto add = [this](std::string method, std::string path, std::string summary, Access access, Handler handler) {
        routes_.push_back({std::move(method), std::move(path), std::move(summary), access, std::move(handler)});
    };
    auto id = [](const Request& req) { return req.path_params.at("id"); };
    const Access Public = Access::Public, Viewer = Access::Viewer, Admin = Access::Admin;

    // ── Authentication ──
    add("GET", "/api/v1/auth/state", "Whether setup is needed and who is signed in", Public,
        [this](const Request& req, Response& res, const Principal&) {
            bool viaCookie = false;
            auto who = authenticate(req, viaCookie);
            sendJson(res, 200, {{"needs_setup", auth_.needsSetup()},
                                {"version", kVersion},
                                {"mode", fleet_ ? "fleet" : "device"},
                                {"user", who ? json{{"name", who->name}, {"role", roleName(who->role)}} : json(nullptr)}});
        });
    add("POST", "/api/v1/auth/setup", "Create the first administrator (needs the setup code from the console)", Public,
        [this](const Request& req, Response& res, const Principal&) {
            json b = body(req);
            auto session = auth_.setup(b.value("setup_code", ""), b.value("username", ""), b.value("password", ""),
                                       req.remote_addr);
            if (!session) throw ApiError(422, session.error() == Auth::kWrongSetupCode ? "wrong_setup_code" : "invalid", session.error());
            setSessionCookie(res, *session);
            sendJson(res, 200, {{"user", {{"name", b.value("username", "")}, {"role", "admin"}}}});
        });
    add("POST", "/api/v1/auth/setup/check", "Check the setup code ({\"code\"}) without using it, before asking for the account", Public,
        [this](const Request& req, Response& res, const Principal&) {
            json b = body(req);
            if (!b.is_object() || !b.contains("code") || !b["code"].is_string()) {
                throw ApiError(400, "bad_request", "send {\"code\": \"<the setup code from the console>\"}");
            }
            // Compared exactly as /auth/setup compares it, so a code that passes here passes there
            Auth::SetupCodeCheck check = auth_.checkSetupCode(b["code"].get<std::string>(), req.remote_addr);
            switch (check.result) {
                case Auth::SetupCodeCheck::Ok: sendJson(res, 200, {{"ok", true}}); return;
                case Auth::SetupCodeCheck::Done: throw ApiError(409, "setup_done", check.message);
                case Auth::SetupCodeCheck::Blocked: throw ApiError(429, "too_many_attempts", check.message);
                case Auth::SetupCodeCheck::Wrong: throw ApiError(403, "wrong_setup_code", check.message);
            }
        });
    add("POST", "/api/v1/auth/login", "Sign in; sets a session cookie", Public,
        [this](const Request& req, Response& res, const Principal&) {
            json b = body(req);
            std::string user = b.value("username", "");
            auto session = auth_.login(user, b.value("password", ""), req.remote_addr);
            if (!session) throw ApiError(401, "unauthorized", session.error());
            setSessionCookie(res, *session);
            auto principal = auth_.session(*session);
            sendJson(res, 200, {{"user", {{"name", user}, {"role", roleName(principal ? principal->role : Role::Viewer)}}}});
        });
    add("POST", "/api/v1/auth/logout", "Sign out", Public, [this](const Request& req, Response& res, const Principal&) {
        auth_.logout(cookieValue(req, kSessionCookie));
        setSessionCookie(res, "", true);
        sendJson(res, 200, {{"ok", true}});
    });

    // ── System ──
    add("GET", "/api/v1/status", "Version, uptime and warnings (a fleet: its devices)", Viewer,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, fleet_ ? fleet_->status() : api_.status()); });
    add("GET", "/api/v1/logs", "Log records (?since=&level=&plc=&limit=)", Viewer,
        [this](const Request& req, Response& res, const Principal&) {
            sendJson(res, 200, api_.logs(static_cast<uint64_t>(intParam(req, "since", 0, 0)), param(req, "level"),
                                         param(req, "plc"), static_cast<size_t>(intParam(req, "limit", 500, 1, 2000))));
        });
    add("GET", "/api/v1/events", "Live log records and PLC summaries (Server-Sent Events)", Viewer,
        [this](const Request& req, Response& res, const Principal&) {
            streamEvents(res, static_cast<uint64_t>(std::max(0, intParam(req, "since", 0))), stillAuthorized(req));
        });
    add("GET", "/api/v1/settings", "Web and log settings", Admin,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, api_.settings()); });
    add("PATCH", "/api/v1/settings", "Change web and log settings", Admin,
        [this](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.updateSettings(body(req), &who)); });
    add("GET", "/api/v1/examples", "Bundled example programs", Viewer,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, api_.examples()); });
    add("GET", "/api/v1/examples/:name", "One example with its programs", Viewer,
        [this](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.example(req.path_params.at("name"))); });
    add("GET", "/api/v1/docs/structured-text", "Structured Text reference (Markdown)", Viewer,
        [this](const Request&, Response& res, const Principal&) {
            res.set_content(api_.languageReference(), "text/markdown; charset=utf-8");
        });
    add("GET", "/api/v1/docs/st-compliance", "IEC 61131-3 compliance statement (Markdown)", Viewer,
        [this](const Request&, Response& res, const Principal&) {
            res.set_content(api_.complianceStatement(), "text/markdown; charset=utf-8");
        });
    add("GET", "/api/v1/openapi.json", "This API description", Public,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, openApi()); });

    // ── The PLC (a device has one; a fleet lists its devices' PLCs) ──
    add("GET", "/api/v1/plcs", "The PLC with state and statistics (a fleet: all its devices' PLCs)", Viewer,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, plcList()); });
    add("GET", "/api/v1/plcs/:id", "The PLC with configuration and statistics", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) { sendJson(res, 200, withVersion(res, api_.getPlc(id(req)))); });
    add("PATCH", "/api/v1/plcs/:id", "Change the PLC (JSON merge patch of its configuration)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, withVersion(res, api_.updatePlc(id(req), body(req), changeOptions(req), &who))); });
    add("POST", "/api/v1/plcs/:id/setup", "Set the PLC up from scratch: name, tags, tasks, program (or an example)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, withVersion(res, api_.setupPlc(id(req), body(req), changeOptions(req), &who))); });
    add("GET", "/api/v1/plcs/:id/device", "The device the PLC runs on: model, OS, CPU, memory, storage, network", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.device(id(req))); });
    add("POST", "/api/v1/plcs/:id/run", "Start the programs (RUN)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.runPlc(id(req), &who)); });
    add("POST", "/api/v1/plcs/:id/stop", "Stop the programs (STOP)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.stopPlc(id(req), &who)); });
    add("POST", "/api/v1/plcs/:id/restart", "Cold restart: reset values, reload programs, RUN", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.restartPlc(id(req), &who)); });
    add("GET", "/api/v1/plcs/:id/events", "Live values and status (Server-Sent Events)", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) { streamPlc(id(req), res, stillAuthorized(req)); });

    // ── Tags and values ──
    add("GET", "/api/v1/plcs/:id/tags", "Tags with current values", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.tags(id(req))); });
    add("PUT", "/api/v1/plcs/:id/tags", "Replace the tag table", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.setTags(id(req), body(req), changeOptions(req), &who)); });
    add("POST", "/api/v1/plcs/:id/tags", "Add a tag", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 201, api_.addTag(id(req), body(req), changeOptions(req), &who)); });
    add("PATCH", "/api/v1/plcs/:id/tags/:name", "Change a tag", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) {
            sendJson(res, 200, api_.updateTag(id(req), req.path_params.at("name"), body(req), changeOptions(req), &who));
        });
    add("DELETE", "/api/v1/plcs/:id/tags/:name", "Delete a tag", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) {
            api_.deleteTag(id(req), req.path_params.at("name"), changeOptions(req), &who);
            sendJson(res, 200, {{"deleted", req.path_params.at("name")}});
        });
    add("GET", "/api/v1/plcs/:id/values", "Tag values (?tags=a,b; all when omitted)", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            std::vector<std::string> names;
            std::string list = param(req, "tags");
            for (size_t pos = 0; pos < list.size();) {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                if (comma > pos) names.push_back(list.substr(pos, comma - pos));
                pos = comma + 1;
            }
            sendJson(res, 200, api_.values(id(req), names));
        });
    add("PUT", "/api/v1/plcs/:id/values", "Write tag values atomically ({\"tag\": value})", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.writeValues(id(req), body(req), &who)); });
    add("GET", "/api/v1/plcs/:id/areas/:area", "Raw entries (?start=&count=)", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            sendJson(res, 200, api_.readArea(id(req), req.path_params.at("area"), intParam(req, "start", 0), intParam(req, "count", 1)));
        });
    add("PUT", "/api/v1/plcs/:id/areas/:area", "Write raw entries ({\"start\", \"values\"})", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) {
            sendJson(res, 200, api_.writeArea(id(req), req.path_params.at("area"), body(req), &who));
        });
    add("GET", "/api/v1/plcs/:id/forces", "Forced entries", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.forces(id(req))); });
    add("POST", "/api/v1/plcs/:id/forces", "Force tags ({\"tag\": value})", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.force(id(req), body(req), &who)); });
    add("DELETE", "/api/v1/plcs/:id/forces", "Release forces ({\"tags\": [...]} or all)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.release(id(req), body(req), &who)); });

    // ── Tasks and programs ──
    add("GET", "/api/v1/plcs/:id/tasks", "Tasks with cycle statistics and errors", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.tasks(id(req))); });
    add("PATCH", "/api/v1/plcs/:id/tasks/:task", "Change a task (cycle_ms, program, enabled)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) {
            sendJson(res, 200, api_.updateTask(id(req), req.path_params.at("task"), body(req), changeOptions(req), &who));
        });
    add("GET", "/api/v1/plcs/:id/variables", "Program variables of a task (?task=&prefix=)", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            sendJson(res, 200, api_.variables(id(req), param(req, "task"), param(req, "prefix")));
        });
    add("PUT", "/api/v1/plcs/:id/variables", "Write program variables ({\"task\", \"values\": {\"MAIN.X\": 1}})", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.setVariables(id(req), body(req), &who)); });
    add("GET", "/api/v1/plcs/:id/programs", "Programs", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.programs(id(req))); });
    add("GET", "/api/v1/plcs/:id/programs/:name", "Program source (text; ETag: its version for If-Match)", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            std::string source = api_.programSource(id(req), req.path_params.at("name"));
            res.set_header("ETag", "\"" + sourceHash(source) + "\"");
            res.set_content(source, "text/plain; charset=utf-8");
        });
    add("PUT", "/api/v1/plcs/:id/programs/:name",
        "Save a program and load it (?deploy=false to only save; If-Match: the ETag read, 412 if it changed)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) {
            sendJson(res, 200, api_.saveProgram(id(req), req.path_params.at("name"), sourceFrom(req), boolParam(req, "deploy", true),
                                                ifMatch(req), &who));
        });
    add("GET", "/api/v1/plcs/:id/programs/:name/deployed", "The files the tasks run the program with (what a restart runs)", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            sendJson(res, 200, api_.deployedProgram(id(req), req.path_params.at("name")));
        });
    add("POST", "/api/v1/plcs/:id/programs/:name/check", "Compile source as this program without saving it (text/plain or {\"source\"})", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            // {"source", "tags"}: check against other tags than the PLC's (e.g. before setting them up)
            json tags = nullptr;
            if (req.get_header_value("Content-Type").starts_with("application/json")) {
                json b = body(req);
                if (b.is_object() && b.contains("tags")) tags = b["tags"];
            }
            sendJson(res, 200, api_.checkProgram(id(req), req.path_params.at("name"), sourceFrom(req), tags));
        });
    add("GET", "/api/v1/plcs/:id/programs/:name/compiled", "The program as compiled: diagnostics, memory and listing", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            sendJson(res, 200, api_.compiledProgram(id(req), req.path_params.at("name")));
        });
    add("DELETE", "/api/v1/plcs/:id/programs/:name", "Delete a program no task runs and no program uses (its history keeps it)", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) {
            api_.deleteProgram(id(req), req.path_params.at("name"), &who);
            sendJson(res, 200, {{"deleted", req.path_params.at("name")}});
        });
    add("GET", "/api/v1/plcs/:id/programs/:name/history", "Earlier versions of a program", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            sendJson(res, 200, api_.programHistory(id(req), req.path_params.at("name")));
        });
    add("GET", "/api/v1/plcs/:id/programs/:name/history/:version", "Source of an earlier version", Viewer,
        [this, id](const Request& req, Response& res, const Principal&) {
            res.set_content(api_.programVersion(id(req), req.path_params.at("name"), req.path_params.at("version")),
                            "text/plain; charset=utf-8");
        });
    add("POST", "/api/v1/plcs/:id/programs/:name/restore", "Restore an earlier version ({\"version\"})", Admin,
        [this, id](const Request& req, Response& res, const Principal& who) {
            json b = body(req);
            sendJson(res, 200, api_.restoreProgram(id(req), req.path_params.at("name"), b.value("version", ""), b.value("deploy", true), &who));
        });

    // ── Users and tokens ──
    add("GET", "/api/v1/users", "Users", Admin,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, api_.users()); });
    add("POST", "/api/v1/users", "Add a user ({name, password, role})", Admin,
        [this](const Request& req, Response& res, const Principal& who) { sendJson(res, 201, api_.addUser(body(req), &who)); });
    add("DELETE", "/api/v1/users/:name", "Remove a user", Admin,
        [this](const Request& req, Response& res, const Principal& who) {
            sendJson(res, 200, api_.removeUser(req.path_params.at("name"), who));
        });
    add("PUT", "/api/v1/users/:name/password", "Change a password (your own, or any as administrator)", Viewer,
        [this](const Request& req, Response& res, const Principal& who) {
            sendJson(res, 200, api_.setPassword(req.path_params.at("name"), body(req), who));
        });
    add("GET", "/api/v1/tokens", "API tokens", Admin,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, api_.tokens()); });
    add("POST", "/api/v1/tokens", "Create an API token ({name, role}); the token is shown once", Admin,
        [this](const Request& req, Response& res, const Principal& who) { sendJson(res, 201, api_.createToken(body(req), who)); });
    add("DELETE", "/api/v1/tokens/:token", "Revoke an API token", Admin,
        [this](const Request& req, Response& res, const Principal& who) {
            api_.revokeToken(req.path_params.at("token"), &who);
            sendJson(res, 200, {{"revoked", req.path_params.at("token")}});
        });

    // ── Events and probing ──
    // The PLC's events share the route of its live stream: ?limit= or Accept: application/json asks for the list
    for (Route& route : routes_) {
        if (route.path != "/api/v1/plcs/:id/events") continue;
        route.summary += "; with ?limit=N (default 50, at most 500) or Accept: application/json the newest events instead: "
                         "changes and changes of state, with who made them";
        route.handler = [this, stream = route.handler](const Request& req, Response& res, const Principal& who) {
            if (!wantsEventList(req)) return stream(req, res, who);
            sendJson(res, 200, api_.events(req.path_params.at("id"), static_cast<size_t>(intParam(req, "limit", 50, 1, 500))));
        };
    }
    add("POST", "/api/v1/plcs/:id/probe",
        "Read a Modbus TCP device from this PLC's network: identification and up to 64 entries of each area from 0; never writes "
        "({host, port, unit, timeout_ms})", Admin,
        [this, id](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.probe(id(req), body(req))); });

    // ── Adoption by a fleet ──
    add("GET", "/api/v1/hello", "What this is: {product, version, plc, needs_setup}, for a fleet adopting it", Public,
        [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, api_.hello()); });
    add("POST", "/api/v1/pairing", "A pairing code a fleet adopts this device with (8 characters, 10 minutes, once)", Admin,
        [this](const Request&, Response& res, const Principal& who) { sendJson(res, 200, api_.pairingCode(&who)); });
    add("POST", "/api/v1/pair", "Trade a pairing code for an admin token ({code, fleet}): what a fleet does to adopt this device", Public,
        [this](const Request& req, Response& res, const Principal&) {
            // Limited like the setup code: per address, wrong codes make it wait
            if (auto wait = auth_.blockedFor(Attempt::Password, req.remote_addr)) {
                throw ApiError(429, "too_many_attempts", std::format("too many wrong codes from this address; try again in {} seconds", wait));
            }
            sendJson(res, 200, api_.pair(body(req), req.remote_addr));
        });
    add("DELETE", "/api/v1/pair", "Leave the fleet: revokes the token this request signs in with", Admin,
        [this](const Request& req, Response& res, const Principal& who) { sendJson(res, 200, api_.unpair(bearerToken(req).value_or(""), &who)); });

    // ── Modbus client ──
    add("POST", "/api/v1/modbus/read", "Read from any Modbus TCP device ({host, port, area, address, count})", Admin,
        [this](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.modbusRead(body(req))); });
    add("POST", "/api/v1/modbus/write", "Write to any Modbus TCP device ({host, port, area, address, values})", Admin,
        [this](const Request& req, Response& res, const Principal&) { sendJson(res, 200, api_.modbusWrite(body(req))); });

    if (fleet_) {
        add("GET", "/api/v1/fleet/devices", "The fleet's devices: [{url, plc, online, error, source}]", Viewer,
            [this](const Request&, Response& res, const Principal&) { sendJson(res, 200, fleet_->devices()); });
        add("POST", "/api/v1/fleet/devices", "Adopt a device: {url, code} (its pairing code) or {url, token}", Admin,
            [this](const Request& req, Response& res, const Principal&) { sendJson(res, 201, fleetCall(fleet_->addDevice(body(req)))); });
        add("DELETE", "/api/v1/fleet/devices/:plc", "Remove a device (its token is revoked on it when it is reachable)", Admin,
            [this](const Request& req, Response& res, const Principal&) { sendJson(res, 200, fleetCall(fleet_->removeDevice(req.path_params.at("plc")))); });
    }

    // A fleet has no PLC of its own: every route about one PLC goes to the device that runs it,
    // checked by the same access rules (the route table) before it leaves the fleet
    if (fleet_) {
        for (Route& route : routes_) {
            if (route.path == "/api/v1/plcs/:id/events") {
                route.handler = [this, path = route.path](const Request& req, Response& res, const Principal& who) {
                    if (wantsEventList(req)) return proxyToDevice(req, res, path, who);
                    streamFromDevice(req, res);
                };
            } else if (route.path.starts_with("/api/v1/plcs/:id")) {
                route.handler = [this, path = route.path](const Request& req, Response& res, const Principal& who) {
                    proxyToDevice(req, res, path, who);
                };
            }
        }
    }
}

}  // namespace simpleplc::web
