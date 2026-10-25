// SimplePLC: soft PLC with Structured Text programs, Modbus TCP, OPC UA, a web panel, a REST API and MCP.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "app/config.h"
#include "app/engine.h"
#include "app/log.h"
#include "app/version.h"
#include "st/compiler.h"
#include "st/outline.h"
#include "net/socket.h"
#include "runtime/program_store.h"
#include "runtime/task.h"
#include "web/api_service.h"
#include "web/auth.h"
#include "web/fleet.h"
#include "web/mcp.h"
#include "web/web_server.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace simpleplc;

namespace {

std::atomic<bool> stopRequested{false};

void onSignal(int) { stopRequested = true; }

void installSignalHandlers() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onSignal);
#endif
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

constexpr const char* kUsage = R"(SimplePLC {} - a soft PLC: IEC 61131-3 Structured Text, Modbus TCP, OPC UA, a web panel and MCP

One SimplePLC is one PLC, like a hardware PLC or a container running a virtual PLC.
Run several (processes, containers, Raspberry Pis) for a network of PLCs.

Usage:
  simpleplc [options]                 run the PLC (default)
  simpleplc fleet --device URL...     one panel and MCP endpoint for the PLCs of many devices
  simpleplc mcp [options]             MCP server on stdin/stdout, with its own PLC: blank on first run,
                                      Modbus and OPC UA on 127.0.0.1 unless --listen, and no web panel
                                      unless --port or --web
  simpleplc mcp --url URL --token T   MCP on stdin/stdout for a running SimplePLC or fleet
  simpleplc check FILE.st...          compile Structured Text and print every problem (--listing: as compiled)
        The first file holds the PROGRAM, the others are libraries. --tags FILE checks VAR_EXTERNAL
        against the tags in FILE (a tag array, an example's plc.json or a config.json) as on a PLC.
  simpleplc install-service           run SimplePLC at login (systemd user unit or launchd agent); with sudo on
                                      Linux: a system service that starts at boot and may use port 502
  simpleplc --version | --help

Options:
  --data DIR       data directory (default: ./simpleplc-data, or ~/.simpleplc; env SIMPLEPLC_DATA);
                   one process at a time uses it
  --port N         web panel port (default 8080; env SIMPLEPLC_WEB_PORT)
  --listen ADDR    web panel address (default 0.0.0.0; mcp: 127.0.0.1); on first run also Modbus's and OPC UA's
  --web            mcp only: also serve the web panel (on 127.0.0.1 unless --listen)
  --no-web         mcp only: no web panel (the default)
  -v, --verbose    show debug messages
  -q, --quiet      show only warnings and errors
  --log-json       print log records as JSON lines

First run (a new data directory starts with these; later, change them in the panel or API):
  --id ID           PLC id (default: the host name; env SIMPLEPLC_ID)
  --name NAME       display name (env SIMPLEPLC_NAME)
  --example NAME    start from an example (tank, blink, traffic-light, remote-io, factory-io-conveyor);
                    default "none": a blank PLC (env SIMPLEPLC_EXAMPLE)
  --modbus-port N   Modbus TCP port (default 502; env SIMPLEPLC_MODBUS_PORT)
  --opcua-port N    OPC UA port (default 4840; env SIMPLEPLC_OPCUA_PORT)

Fleet:
  --device URL      a device's web address, e.g. http://tank:8080 or tank (repeat; env SIMPLEPLC_DEVICES,
                    comma-separated); the fleet signs in to devices with SIMPLEPLC_DEVICE_TOKEN
                    (default: SIMPLEPLC_TOKEN). Without --device a fleet starts empty: adopt devices with
                    their pairing code (kept in <data>/fleet.json)

Environment:
  SIMPLEPLC_TOKEN  an administrator API token for scripts and agents (never stored)
)";

struct Options {
    std::string command = "run";
    std::optional<fs::path> dataDir;
    std::optional<int> webPort;
    std::optional<std::string> webListen;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<std::string> example;
    std::optional<int> modbusPort;
    std::optional<int> opcuaPort;
    std::vector<std::string> devices;
    bool noWeb = false;
    bool web = false;  // mcp: serve the web panel too
    std::optional<log::Level> consoleLevel;
    bool jsonLog = false;
    std::string url;
    std::string token;
    std::vector<std::string> files;
    bool listing = false;
    std::string tagsFile;
};

[[noreturn]] void fail(const std::string& message, int code = 2) {
    std::fprintf(stderr, "simpleplc: %s\n", message.c_str());
    std::exit(code);
}

Options parseArgs(int argc, char** argv) {
    Options o;
    int i = 1;
    if (i < argc && argv[i][0] != '-') o.command = argv[i++];
    // The command first: `simpleplc foo --bar` is an unknown command, not an unknown option
    static constexpr const char* kCommands[] = {"run", "fleet", "mcp", "check", "install-service"};
    if (std::find(std::begin(kCommands), std::end(kCommands), o.command) == std::end(kCommands)) {
        fail(std::format("unknown command '{}' (see simpleplc --help)", o.command));
    }
    for (; i < argc; i++) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) fail(std::format("{} needs a value", arg));
            return argv[++i];
        };
        auto number = [&]() -> int {
            std::string text = value();
            try {
                return std::stoi(text);
            } catch (...) {
                fail(std::format("{} needs a number", arg));
            }
        };
        if (arg == "--help" || arg == "-h") o.command = "help";
        else if (arg == "--version" || arg == "-V") o.command = "version";
        else if (arg == "--data") o.dataDir = value();
        else if (arg == "--port") o.webPort = number();
        else if (arg == "--listen") o.webListen = value();
        else if (arg == "--id") o.id = value();
        else if (arg == "--name") o.name = value();
        else if (arg == "--example") o.example = value();
        else if (arg == "--modbus-port") o.modbusPort = number();
        else if (arg == "--opcua-port") o.opcuaPort = number();
        else if (arg == "--device") o.devices.push_back(value());
        else if (arg == "--no-web") o.noWeb = true;
        else if (arg == "--web") o.web = true;
        else if (arg == "-v" || arg == "--verbose") o.consoleLevel = log::Level::Debug;
        else if (arg == "-q" || arg == "--quiet") o.consoleLevel = log::Level::Warn;
        else if (arg == "--log-json") o.jsonLog = true;
        else if (arg == "--url") o.url = value();
        else if (arg == "--token") o.token = value();
        else if (arg == "--listing") o.listing = true;
        else if (arg == "--tags") o.tagsFile = value();
        else if (!arg.empty() && arg[0] != '-') o.files.push_back(arg);
        else fail(std::format("unknown option '{}' (see simpleplc --help)", arg));
    }
    return o;
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

/// A port from the command line, else from the environment, else the default.
int portOption(const std::optional<int>& option, const char* envName, int fallback) {
    if (option) return *option;
    std::string e = env(envName);
    if (e.empty()) return fallback;
    try {
        return std::stoi(e);
    } catch (...) {
        fail(std::format("{} must be a number", envName));
    }
}

Engine::FirstRun firstRunOptions(const Options& o) {
    Engine::FirstRun f;
    f.id = o.id.value_or(env("SIMPLEPLC_ID"));
    if (!f.id.empty() && !isValidPlcId(f.id)) {
        fail(std::format("PLC id '{}' is invalid: lowercase letters, digits, - and _, starting with a letter", f.id));
    }
    f.name = o.name.value_or(env("SIMPLEPLC_NAME"));
    // A user starts with an empty PLC; an example only when asked for
    std::string example = o.example ? *o.example : env("SIMPLEPLC_EXAMPLE");
    f.example = example == "none" ? "" : example;
    f.modbusPort = portOption(o.modbusPort, "SIMPLEPLC_MODBUS_PORT", 502);
    f.opcuaPort = portOption(o.opcuaPort, "SIMPLEPLC_OPCUA_PORT", 4840);
    for (auto [what, port] : {std::pair{"--modbus-port", f.modbusPort}, std::pair{"--opcua-port", f.opcuaPort}}) {
        if (port < 1 || port > 65535) fail(std::format("{} must be 1-65535", what));
    }
    f.listen = o.webListen.value_or("");  // a device started for loopback only keeps Modbus and OPC UA there too
    // An agent's private PLC (simpleplc mcp) is local unless told otherwise
    if (o.command == "mcp" && f.listen.empty()) f.listen = "127.0.0.1";
    return f;
}

/// First-run options given for a data directory that already has its configuration: they would be
/// ignored, so say so (the ports especially: the PLC listens where its configuration says).
void warnIgnoredFirstRunOptions(const Options& o, const fs::path& dataDir) {
    std::error_code ec;
    if (!fs::exists(dataDir / "config.json", ec)) return;
    std::string given;
    for (auto [flag, set] : {std::pair{"--id", o.id.has_value()}, std::pair{"--name", o.name.has_value()},
                             std::pair{"--example", o.example.has_value()}, std::pair{"--modbus-port", o.modbusPort.has_value()},
                             std::pair{"--opcua-port", o.opcuaPort.has_value()}}) {
        if (set) given += (given.empty() ? "" : ", ") + std::string(flag);
    }
    if (given.empty()) return;
    log::write(log::Level::Warn, "main", "",
               std::format("{} only apply to a new data directory; {} already has its configuration, so they are ignored "
                           "(change the PLC in the panel, the API or MCP, or use a new --data directory)", given, dataDir.string()));
}

fs::path homeDataDir() {
#ifdef _WIN32
    std::string appData = env("APPDATA");
    if (!appData.empty()) return fs::path(appData) / "SimplePLC";
#endif
    std::string home = env("HOME");
    if (!home.empty()) return fs::path(home) / ".simpleplc";
    return fs::current_path() / "simpleplc-data";
}

fs::path resolveDataDir(const Options& o, const char* folder = "simpleplc-data") {
    if (o.dataDir) return fs::absolute(*o.dataDir);
    if (std::string e = env("SIMPLEPLC_DATA"); !e.empty()) return fs::absolute(e);
    std::error_code ec;
    fs::path local = fs::current_path(ec) / folder;
    if (fs::exists(local, ec)) return local;
    // Use the current directory when we may write there, otherwise the home directory
    fs::path probe = fs::current_path(ec) / ".simpleplc-write-test";
    if (runtime::writeFileAtomic(probe, "")) {
        fs::remove(probe, ec);
        return local;
    }
    return homeDataDir() / (std::string_view(folder) == "simpleplc-data" ? "" : folder);
}

fs::path executablePath() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(std::wstring(buffer, n));
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) return fs::canonical(buffer);
    return "simpleplc";
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path("simpleplc") : p;
#endif
}

void configureLogging(const Options& o, log::Level defaultConsole) {
    log::setConsoleLevel(o.consoleLevel.value_or(defaultConsole));
    if (o.consoleLevel == log::Level::Debug) log::setLevel(log::Level::Debug);
    log::setJsonConsole(o.jsonLog);
}

/// Everything a running SimplePLC consists of.
struct Runtime {
    std::unique_ptr<Engine> engine;
    std::unique_ptr<web::Auth> auth;
    std::unique_ptr<web::ApiService> api;
    std::unique_ptr<web::McpServer> mcp;
    std::unique_ptr<web::WebServer> web;
    int webPort = 0;
    std::string webListen;

    ~Runtime() { stop(); }

    /// The clean stop: the web panel first (its requests end), then the PLC (its links send the substitute values).
    /// The objects stay usable afterwards (a closed engine answers "not open"), so a late caller finds no freed memory.
    void stop() {
        if (stopped) return;
        stopped = true;
        auto timed = [](const char* what, auto&& fn) {
            auto start = std::chrono::steady_clock::now();
            fn();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            log::write(log::Level::Debug, "main", "", std::format("{} stopped in {} ms", what, ms));
        };
        if (web) timed("web panel", [&] { web->stop(); });
        if (auth) auth->flush(true);  // the tokens' last_used, kept in memory between flushes
        if (engine) timed("PLCs", [&] { engine->close(); });
    }

private:
    bool stopped = false;
};

std::unique_ptr<Runtime> startRuntime(const Options& o, bool withWeb) {
    auto rt = std::make_unique<Runtime>();
    fs::path dataDir = resolveDataDir(o);
    warnIgnoredFirstRunOptions(o, dataDir);
    rt->engine = std::make_unique<Engine>(dataDir, firstRunOptions(o));
    if (auto ok = rt->engine->open(); !ok) {
        std::string message = std::format("cannot start with {}:", (dataDir / "config.json").string());
        for (const auto& p : ok.error()) message += "\n  - " + p;
        fail(message);
    }
    rt->auth = std::make_unique<web::Auth>(dataDir / "users.json", env("SIMPLEPLC_TOKEN"));
    if (auto ok = rt->auth->load(); !ok) fail(ok.error());
    rt->api = std::make_unique<web::ApiService>(rt->engine.get(), *rt->auth);
    rt->mcp = std::make_unique<web::McpServer>(*rt->api);

    if (withWeb) {
        Config config = rt->engine->config();
        int port = portOption(o.webPort, "SIMPLEPLC_WEB_PORT", config.web.port);
        // An agent's private PLC (simpleplc mcp) serves its panel on this computer only, unless told otherwise
        rt->webListen = o.webListen.value_or(o.command == "mcp" ? std::string("127.0.0.1") : config.web.listen);
        rt->web = std::make_unique<web::WebServer>(*rt->api, *rt->auth, *rt->mcp);
        auto started = rt->web->start(rt->webListen, port, true);
        if (!started) {
            log::write(log::Level::Error, "web", "", "web panel not available: " + started.error());
            rt->web.reset();
        } else {
            rt->webPort = *started;
            if (rt->webPort != port) {
                log::write(log::Level::Warn, "web", "",
                           std::format("port {} is in use, so the web panel uses port {}", port, rt->webPort));
            }
            bool loopback = rt->webListen == "127.0.0.1" || rt->webListen == "::1" || rt->webListen == "localhost";
            if (std::string token = env("SIMPLEPLC_TOKEN"); !token.empty() && token.size() < 16 && !loopback) {
                log::write(log::Level::Warn, "web", "",
                           "SIMPLEPLC_TOKEN is short and the web panel is reachable from the network; "
                           "use at least 16 random characters (for example: openssl rand -hex 16)");
            }
        }
    }
    return rt;
}

void printBanner(Runtime& rt, FILE* out) {
    std::string lan = net::primaryAddress();
    auto print = [&](const std::string& line) {
        std::fputs(line.c_str(), out);
        std::fputc('\n', out);
    };
    print(std::format("SimplePLC {}", kVersion));
    print(std::format("  Data       {}", rt.engine->dataDir().string()));
    if (rt.web) {
        bool everywhere = rt.webListen == "0.0.0.0" || rt.webListen == "::";
        std::string host = everywhere ? lan : rt.webListen;
        std::string line = std::format("  Web panel  http://{}:{}", host, rt.webPort);
        std::error_code ec;
        if (fs::exists("/.dockerenv", ec)) {
            // The container's own address is not reachable from the host; the published port is
            line = std::format("  Web panel  port {} in the container (e.g. http://localhost:{} with -p {}:{})",
                               rt.webPort, rt.webPort, rt.webPort, rt.webPort);
        } else if (everywhere && host != "localhost") {
            line += std::format("   (on this computer: http://localhost:{})", rt.webPort);
        }
        print(line);
        if (rt.auth->needsSetup()) {
            print(std::format("  Setup code {}   (enter it in the web panel to create your account)", rt.auth->setupToken()));
        }
    }
    if (auto instance = rt.engine->instance()) {
        PlcConfig c = instance->config();
        ServerStatus mb = instance->modbusStatus();
        ServerStatus ua = instance->opcuaStatus();
        std::string modbus = mb.listening ? std::format("Modbus :{}", mb.port) : "Modbus off";
        std::string opcua = ua.listening ? std::format("OPC UA :{}", ua.port) : "OPC UA off";
        print(std::format("  PLC        {}  {}  {}  [{}]", c.id == c.name ? c.id : std::format("{} ({})", c.id, c.name),
                          modbus, opcua, instance->running() ? "RUN" : "STOP"));
        for (const auto& w : instance->warnings()) print(std::format("  ! {}", w));
    }
    print("Ready. Press Ctrl+C to stop.");
    std::fflush(out);
}

int runServer(const Options& o) {
    configureLogging(o, log::Level::Warn);
    auto rt = startRuntime(o, true);
    printBanner(*rt, stdout);
    log::setConsoleLevel(o.consoleLevel.value_or(log::Level::Info));
    while (!stopRequested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rt->auth->flush();  // the tokens' last_used, once a minute, off the request path
        rt->engine->saveRetained();  // retentive memory, when it changed, at most once a second
    }
    std::fputs("Stopping...\n", stdout);
    rt.reset();
    std::fputs("Stopped.\n", stdout);
    return 0;
}

int runMcpStdio(const Options& o) {
    // stdout carries the protocol; everything else goes to stderr
    configureLogging(o, log::Level::Warn);

    if (!o.url.empty()) {
        std::string token = o.token.empty() ? env("SIMPLEPLC_TOKEN") : o.token;
        if (token.empty()) fail("mcp --url needs --token (or SIMPLEPLC_TOKEN)");
        httplib::Client client(o.url);
        client.set_connection_timeout(std::chrono::seconds(5));
        client.set_read_timeout(std::chrono::seconds(60));
        httplib::Headers headers = {{"Authorization", "Bearer " + token}};
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            auto res = client.Post("/mcp", headers, line, "application/json");
            if (res && res->status == 202) continue;
            if (res && res->status == 200) {
                std::fputs(res->body.c_str(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
                continue;
            }
            json request = web::parseJson(line);
            json error;
            if (request.is_discarded()) {
                // Not JSON: the server would say the same, and the client must hear something (JSON-RPC 5.1)
                error = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error: the line is not valid JSON"}}}};
            } else {
                if (!request.is_object() || !request.contains("id")) continue;
                std::string why = res ? std::format("SimplePLC answered HTTP {}: {}", res->status, res->body)
                                      : std::format("cannot reach {}: {}", o.url, httplib::to_string(res.error()));
                error = {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"error", {{"code", -32603}, {"message", why}}}};
            }
            std::fputs((web::dumpJson(error) + "\n").c_str(), stdout);
            std::fflush(stdout);
        }
        return 0;
    }

    // An agent's private PLC: the web panel only when asked for (--web or --port)
    auto rt = startRuntime(o, (o.web || o.webPort.has_value()) && !o.noWeb);
    printBanner(*rt, stderr);
    // The reader blocks in getline, so on SIGINT or SIGTERM it cannot be joined: it stays until the process
    // exits. It must then never touch the runtime again: both sides go through this gate. The reader
    // answers a line only while the gate is open; the main thread closes it (waiting for an answer in
    // progress) before the runtime is destroyed, so a line that arrives during or after the shutdown
    // is dropped instead of reaching a freed McpServer
    struct Gate {
        std::timed_mutex mutex;
        bool open = true;
        bool readerDone = false;  // the reader left its loop: it can be joined
    };
    auto gate = std::make_shared<Gate>();
    std::thread reader([mcp = rt->mcp.get(), gate] {
        web::Principal local{"mcp-stdio", web::Role::Admin, true, {}, {}};
        auto answer = [&](const json& message) -> std::optional<json> {
            try {
                return mcp->handle(message, local);
            } catch (...) {
                // handle() answers everything itself; this keeps the PLC running whatever happens
                return json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32603}, {"message", "Internal error"}}}};
            }
        };
        std::string line;
        bool closed = false;
        while (!closed && std::getline(std::cin, line)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            json message = web::parseJson(line);
            std::optional<json> response;
            std::lock_guard lock(gate->mutex);
            if (!gate->open) {
                closed = true;  // stopping: the runtime is going away
                break;
            }
            if (message.is_discarded()) {
                response = json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error"}}}};
            } else if (message.is_array()) {
                // A JSON-RPC batch (protocol versions before 2025-06-18): each element answered on its own
                json responses = json::array();
                for (const json& m : message) {
                    if (auto r = answer(m)) responses.push_back(*r);
                }
                if (message.empty()) {
                    response = json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32600}, {"message", "Invalid Request: an empty batch"}}}};
                } else if (!responses.empty()) {
                    response = responses;
                }
            } else {
                response = answer(message);
            }
            if (response) {
                std::fputs((web::dumpJson(*response) + "\n").c_str(), stdout);
                std::fflush(stdout);
            }
        }
        {
            std::lock_guard lock(gate->mutex);
            gate->readerDone = true;
        }
        stopRequested = true;  // the client closed stdin (or the PLC is stopping anyway)
    });
    while (!stopRequested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rt->auth->flush();
        rt->engine->saveRetained();
    }
    // An answer in progress ends first, within a bound: a request that outlasts it (a probe of a silent device
    // can take 20 s) must not hold up the stop. The runtime is then stopped but kept alive, not destroyed, so
    // what the reader still does reads valid memory (a closed engine answers "not open"), and the process exits
    std::unique_lock<std::timed_mutex> gateLock(gate->mutex, std::defer_lock);
    bool closed = gateLock.try_lock_for(std::chrono::seconds(5));
    bool joinable = false;
    if (closed) {
        gate->open = false;
        joinable = gate->readerDone;
        gateLock.unlock();
    }
    if (joinable) reader.join();
    else reader.detach();  // blocked in getline: it ends with the process, and the gate keeps it off the runtime
    rt->stop();  // a clean stop: the links send the substitute values, the servers close
    if (closed) rt.reset();
    else (void)rt.release();  // the reader may still be answering: nothing it uses is freed before the exit
    return 0;
}

int runFleet(const Options& o) {
    configureLogging(o, log::Level::Warn);
    std::vector<std::string> urls = o.devices;
    if (urls.empty()) {
        std::string list = env("SIMPLEPLC_DEVICES");
        for (size_t pos = 0; pos < list.size();) {
            size_t comma = list.find(',', pos);
            if (comma == std::string::npos) comma = list.size();
            std::string url = list.substr(pos, comma - pos);
            url.erase(0, url.find_first_not_of(" \t"));
            url.erase(url.find_last_not_of(" \t") + 1);
            if (!url.empty()) urls.push_back(url);
            pos = comma + 1;
        }
    }
    // Devices given here are signed in to with the environment's token; adopted ones (with a pairing code, in the
    // panel or the API) bring their own and are kept in fleet.json. A fleet may start with none
    std::vector<web::Fleet::DeviceEntry> devices;
    std::string token = env("SIMPLEPLC_DEVICE_TOKEN");
    if (token.empty()) token = env("SIMPLEPLC_TOKEN");
    if (!urls.empty() && token.empty()) {
        fail("the fleet signs in to the devices given with --device with an admin token: set SIMPLEPLC_DEVICE_TOKEN (or SIMPLEPLC_TOKEN), "
             "or adopt them with a pairing code instead");
    }
    for (std::string& url : urls) {
        auto parsed = web::Fleet::parseDeviceUrl(url);
        if (!parsed) fail(parsed.error());
        url = *parsed;
        devices.push_back({url, token, true});
    }

    // The fleet's own panel users live in its data directory; it has no PLC
    fs::path dataDir = resolveDataDir(o, "simpleplc-fleet");
    std::error_code ec;
    fs::create_directories(dataDir, ec);
    if (ec) fail(std::format("cannot create {}: {}", dataDir.string(), ec.message()));
    auto stored = web::Fleet::loadDevices(dataDir / "fleet.json");
    if (!stored) fail(stored.error());
    devices.insert(devices.end(), stored->begin(), stored->end());
    web::Auth auth(dataDir / "users.json", env("SIMPLEPLC_TOKEN"));
    if (auto ok = auth.load(); !ok) fail(ok.error());
    web::ApiService api(nullptr, auth);
    std::string name = plcIdFromHostname(net::hostname());  // its tokens on the devices are "fleet:<name>"
    web::Fleet fleet(devices, dataDir / "fleet.json", name.empty() ? std::string("fleet") : name);
    web::McpServer mcp(api, &fleet);
    web::WebServer server(api, auth, mcp, &fleet);

    std::string listen = o.webListen.value_or("0.0.0.0");
    // A port given on purpose (compose files, reverse proxies, health checks) is kept or the fleet does not
    // start; only the default moves on to the next free port, and says so
    bool portGiven = o.webPort.has_value() || !env("SIMPLEPLC_WEB_PORT").empty();
    int wanted = portOption(o.webPort, "SIMPLEPLC_WEB_PORT", 8080);
    auto port = server.start(listen, wanted, !portGiven);
    if (!port) {
        fail(portGiven ? std::format("web panel not available: {} (is another fleet or program using port {}? "
                                     "choose another with --port)", port.error(), wanted)
                       : "web panel not available: " + port.error());
    }
    if (*port != wanted) {
        log::write(log::Level::Warn, "web", "", std::format("port {} is in use, so the fleet's web panel uses port {}", wanted, *port));
    }
    fleet.start();  // after anything that may fail: exiting with pollers running is not clean
    std::printf("SimplePLC fleet %s\n", kVersion);
    std::printf("  Data       %s\n", dataDir.string().c_str());
    bool everywhere = listen == "0.0.0.0" || listen == "::";
    std::printf("  Web panel  http://%s:%d   (MCP: /mcp)\n", (everywhere ? net::primaryAddress() : listen).c_str(), *port);
    if (auth.needsSetup()) std::printf("  Setup code %s   (enter it in the web panel to create your account)\n", auth.setupToken().c_str());
    for (const auto& d : devices) std::printf("  Device     %s%s\n", d.url.c_str(), d.flag ? "" : "   (adopted)");
    if (devices.empty()) std::printf("  Devices    none yet: adopt one with its pairing code (panel, or POST /api/v1/fleet/devices)\n");
    std::printf("Ready. Press Ctrl+C to stop.\n");
    std::fflush(stdout);
    log::setConsoleLevel(o.consoleLevel.value_or(log::Level::Info));
    while (!stopRequested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auth.flush();  // the tokens' last_used, once a minute, off the request path
    }
    std::fputs("Stopping...\n", stdout);
    // The fleet first: it ends every request still waiting for a device, so the web server's workers
    // (which it waits for) do not hang on a stuck device
    fleet.stop();
    server.stop();
    auth.flush(true);
    std::fputs("Stopped.\n", stdout);
    return 0;
}

/// The ST files of a command line: the first holds the PROGRAM, the others are libraries.
std::vector<st::SourceFile> readSources(const Options& o) {
    std::vector<st::SourceFile> files;
    for (const std::string& file : o.files) {
        std::error_code ec;
        if (fs::is_directory(file, ec)) fail(std::format("{} is a directory: give the .st files (e.g. {}/*.st)", file, file));
        auto source = runtime::readFile(file);
        if (!source) fail(std::format("{}: {}", file, source.error()));
        files.push_back({fs::path(file).filename().string(), *source});
    }
    // Diagnostics name files by their name; two files of one name (a/util.st, b/util.st) by their path
    for (size_t i = 0; i < files.size(); i++) {
        for (size_t j = 0; j < files.size(); j++) {
            if (i != j && fs::path(o.files[i]).filename() == fs::path(o.files[j]).filename()) files[i].name = o.files[i];
        }
    }
    return files;
}

/// --tags: a tag array, an object with "tags" (an example's plc.json) or a config.json ({"plc": {"tags"}}).
std::vector<TagDef> readTags(const Options& o) {
    if (o.tagsFile.empty()) return {};
    auto text = runtime::readFile(o.tagsFile);
    if (!text) fail(std::format("{}: {}", o.tagsFile, text.error()));
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded()) fail(std::format("{}: not valid JSON", o.tagsFile));
    if (j.is_object() && j.contains("plc") && j["plc"].is_object()) j = j["plc"];
    if (j.is_object() && j.contains("tags")) j = j["tags"];
    auto tags = tagsFromJson(j);
    if (!tags) {
        std::string problems;
        for (const std::string& p : tags.error()) problems += "\n  " + p;
        fail(std::format("{}: invalid tags:{}", o.tagsFile, problems));
    }
    return *tags;
}

int checkFiles(const Options& o) {
    if (o.files.empty()) fail("give the program's .st file (and library files)");
    std::vector<st::SourceFile> files = readSources(o);
    // A first file without a PROGRAM is a library: its declarations are checked (with the other files it uses)
    bool library = !st::outline(files[0].text).hasProgram;
    st::CompileResult r = st::compile(files, readTags(o), library ? st::CompileMode::Library : st::CompileMode::Program);
    for (const st::Diagnostic& d : r.diagnostics) std::printf("%s\n", st::formatDiagnostic(d, r.fileNames).c_str());
    if (r.ok && library) {
        std::printf("%s: ok (a library: no PROGRAM)\n", o.files[0].c_str());
        if (o.listing) std::fputs(r.listing.c_str(), stdout);
    } else if (r.ok) {
        std::printf("%s: ok (%u bytes of data, %u of stack, %u instructions)\n", o.files[0].c_str(), r.dataBytes,
                    r.stackBytes, r.codeInstructions);
        if (o.listing) std::fputs(r.listing.c_str(), stdout);
    }
    return r.ok ? 0 : 1;
}

int installService(const Options& o) {
    fs::path exe = executablePath();
    fs::path dataDir = resolveDataDir(o);
#if defined(__linux__)
    if (geteuid() == 0) {
        // With sudo: a system service that starts at boot without anyone logging in, runs as the person who
        // installed it, and may bind port 502 and use real-time scheduling without setcap on the binary
        std::string user = env("SUDO_USER");
        fs::path data = o.dataDir ? fs::absolute(*o.dataDir) : fs::path("/var/lib/simpleplc");
        std::error_code ec;
        fs::create_directories(data, ec);
        if (ec) fail(std::format("cannot create {}: {}", data.string(), ec.message()));
        if (!user.empty()) {
            if (const passwd* pw = getpwnam(user.c_str())) (void)!chown(data.c_str(), pw->pw_uid, pw->pw_gid);  // glibc marks the result must-use
        }
        fs::path unit = "/etc/systemd/system/simpleplc.service";
        std::string content = std::format(
            "[Unit]\nDescription=SimplePLC soft PLC\nAfter=network-online.target\nWants=network-online.target\n\n"
            "[Service]\nExecStart=\"{}\" --data \"{}\"\n{}"
            "AmbientCapabilities=CAP_NET_BIND_SERVICE CAP_SYS_NICE\nRestart=always\nRestartSec=2\n\n"
            "[Install]\nWantedBy=multi-user.target\n",
            exe.string(), data.string(), user.empty() ? std::string() : std::format("User={}\n", user));
        if (auto ok = runtime::writeFileAtomic(unit, content); !ok) fail(ok.error());
        std::printf("Wrote %s (data in %s)\nStart it now and at every boot with:\n  systemctl daemon-reload\n"
                    "  systemctl enable --now simpleplc\nThe setup code is in: journalctl -u simpleplc\n",
                    unit.string().c_str(), data.string().c_str());
        return 0;
    }
    std::string home = env("HOME");
    if (home.empty()) fail("HOME is not set");
    fs::path unit = fs::path(home) / ".config/systemd/user/simpleplc.service";
    std::string content = std::format(
        "[Unit]\nDescription=SimplePLC soft PLC\nAfter=network-online.target\n\n"
        "[Service]\nExecStart=\"{}\" --data \"{}\"\nRestart=on-failure\n\n[Install]\nWantedBy=default.target\n",
        exe.string(), dataDir.string());
    if (auto ok = runtime::writeFileAtomic(unit, content); !ok) fail(ok.error());
    std::printf("Wrote %s\nStart it now and at every login with:\n  systemctl --user daemon-reload\n"
                "  systemctl --user enable --now simpleplc\n"
                "For a service that starts at boot without a login and may use port 502: sudo simpleplc install-service\n",
                unit.string().c_str());
    return 0;
#elif defined(__APPLE__)
    std::string home = env("HOME");
    if (home.empty()) fail("HOME is not set");
    fs::path plist = fs::path(home) / "Library/LaunchAgents/com.simpleplc.plist";
    std::string content = std::format(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>Label</key><string>com.simpleplc</string>\n"
        "  <key>ProgramArguments</key><array><string>{}</string><string>--data</string><string>{}</string></array>\n"
        "  <key>RunAtLoad</key><true/>\n  <key>KeepAlive</key><true/>\n"
        "  <key>StandardErrorPath</key><string>{}</string>\n"
        "</dict>\n</plist>\n",
        exe.string(), dataDir.string(), (dataDir / "simpleplc.log").string());
    if (auto ok = runtime::writeFileAtomic(plist, content); !ok) fail(ok.error());
    std::printf("Wrote %s\nStart it now and at every login with:\n  launchctl load -w %s\n",
                plist.string().c_str(), plist.string().c_str());
    return 0;
#else
    std::printf("On Windows, add a scheduled task that runs at logon:\n"
                "  schtasks /Create /SC ONLOGON /TN SimplePLC /TR \"\\\"%s\\\" --data \\\"%s\\\"\"\n",
                exe.string().c_str(), dataDir.string().c_str());
    return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    installSignalHandlers();  // first, so an early Ctrl+C or SIGTERM is never lost
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    net::initialize();
    Options o = parseArgs(argc, argv);
    try {
        if (o.command == "help") {
            std::fputs(std::format(kUsage, kVersion).c_str(), stdout);
            return 0;
        }
        if (o.command == "version") {
            std::printf("SimplePLC %s\n", kVersion);
            return 0;
        }
        if (o.command == "run") return runServer(o);
        if (o.command == "fleet") return runFleet(o);
        if (o.command == "mcp") return runMcpStdio(o);
        if (o.command == "check") return checkFiles(o);
        if (o.command == "install-service") return installService(o);
        fail(std::format("unknown command '{}' (see simpleplc --help)", o.command));
    } catch (const std::exception& e) {
        fail(e.what(), 1);
    }
}
