#include "web/api_service.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

#include "app/device_info.h"
#include "app/embedded.h"
#include "st/compiler.h"
#include "st/lexer.h"
#include "st/values.h"
#include "app/examples.h"
#include "app/log.h"
#include "app/version.h"
#include "modbus/pdu.h"
#include "modbus/tcp_client.h"
#include "runtime/task.h"
#include "web/crypto.h"

namespace simpleplc::web {
namespace {

ApiError invalid(const std::string& message, json details = nullptr) {
    return ApiError(422, "invalid", message, std::move(details));
}

ApiError invalid(const std::vector<std::string>& problems) {
    std::string message = problems.size() == 1 ? problems.front() : std::format("{} problems: {}", problems.size(), problems.front());
    return ApiError(422, "invalid", message, {{"problems", problems}});
}

ApiError notFound(const std::string& message) { return ApiError(404, "not_found", message); }

bool nestingWithin(std::string_view text, int maxDepth) {
    int depth = 0;
    bool inString = false;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (inString) {
            if (c == '\\') i++;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > maxDepth) return false;
        } else if (c == ']' || c == '}') {
            depth--;
        }
    }
    return true;
}

const json& requireObject(const json& body, const char* what = "the request body") {
    if (!body.is_object()) throw ApiError(400, "bad_request", std::format("{} must be a JSON object", what));
    return body;
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

json faultJson(const runtime::FaultInfo& f) {
    json called = json::array();
    for (const auto& [file, line] : f.calledFrom) called.push_back({{"file", file}, {"line", line}});
    return {{"task", f.task}, {"program", f.program}, {"code", f.code}, {"message", f.message}, {"file", f.file},
            {"line", f.line}, {"cycle", f.cycle}, {"at", f.atMs}, {"called_from", called}};
}

json taskJson(const TaskStatus& t) {
    const runtime::TaskStats& s = t.stats;
    json j = {{"name", t.config.name},
              {"program", t.config.program},
              {"cycle_ms", t.config.cycleMs},
              {"enabled", t.config.enabled},
              {"priority", t.config.priority},
              {"watchdog_ms", t.config.watchdogMs},
              {"watchdog_effective_ms", s.watchdogMs},  // 0 above: the default, max(100 ms, 10 x cycle_ms)
              {"running", s.running},
              {"loaded", s.hasProgram},
              {"cycles", s.cycles},
              {"exceeded", s.exceeded},
              {"nan_rejected", s.nanRejected},
              {"exec_us", {{"last", round3(s.lastExecUs)}, {"avg", round3(s.avgExecUs)}, {"max", round3(s.maxExecUs)}}},
              {"latency_us",
               {{"last", round3(s.lastLateUs)}, {"max", round3(s.maxLateUs)}, {"p50", round3(s.p50LateUs)},
                {"p99", round3(s.p99LateUs)}, {"p999", round3(s.p999LateUs)}}},
              {"memory", {{"data_bytes", s.dataBytes}, {"stack_bytes", s.stackBytes}, {"instructions", s.codeInstructions}}},
              {"realtime", s.realtime},
              {"loaded_at", s.loadedAtMs}};
    j["fault"] = s.fault ? faultJson(*s.fault) : json(nullptr);
    return j;
}

json diagnosticsJson(const st::CompileResult& r) {
    json out = json::array();
    for (const st::Diagnostic& d : r.diagnostics) {
        out.push_back({{"code", d.code},
                       {"severity", d.severity == st::Severity::Error ? "error" : "warning"},
                       {"file", d.loc.file < r.fileNames.size() ? r.fileNames[d.loc.file] : ""},
                       {"line", d.loc.line},
                       {"column", d.loc.column},
                       {"message", d.message},
                       {"hint", d.hint}});
    }
    return out;
}

json compileJson(const st::CompileResult& r) {
    json j = {{"ok", r.ok}, {"diagnostics", diagnosticsJson(r)}};
    if (r.ok) j["memory"] = {{"data_bytes", r.dataBytes}, {"stack_bytes", r.stackBytes}, {"instructions", r.codeInstructions}};
    return j;
}

/// The first error of a compilation as {line, error} for short answers.
std::pair<uint32_t, std::string> firstError(const st::CompileResult& r) {
    for (const st::Diagnostic& d : r.diagnostics) {
        if (d.severity == st::Severity::Error) return {d.loc.line, std::format("{}: {}", d.code, d.message)};
    }
    return {0, ""};
}

/// Programs a configuration change would stop or cannot run, for the error's details.
json programProblemsJson(const std::vector<ProgramProblem>& problems) {
    json out = json::array();
    for (const ProgramProblem& p : problems) {
        out.push_back({{"task", p.task}, {"program", p.program}, {"error", p.error}, {"diagnostics", diagnosticsJson(p.compiled)}});
    }
    return out;
}

/// A ConfigChange flag given in a request body ({"create": true}); removed from the body.
bool takeFlag(json& body, const char* key) {
    if (!body.is_object() || !body.contains(key)) return false;
    if (!body[key].is_boolean()) throw invalid(std::format("{} must be true or false", key));
    bool value = body[key].get<bool>();
    body.erase(key);
    return value;
}

/// At most this many names in an answer's "reset" (a first load lists every variable).
constexpr size_t kMaxResetListed = 500;

void putReset(json& out, const std::vector<std::string>& reset) {
    if (reset.size() <= kMaxResetListed) {
        out["reset"] = reset;
        return;
    }
    out["reset"] = std::vector<std::string>(reset.begin(), reset.begin() + kMaxResetListed);
    out["reset_more"] = reset.size() - kMaxResetListed;
}

Area areaArg(const std::string& name) {
    auto area = parseArea(name);
    if (!area) {
        throw notFound(std::format("unknown area '{}'; use coil, discrete_input, holding_register or input_register", name));
    }
    return *area;
}

std::string hostFor(const std::string& listen) {
    return listen.empty() || listen == "0.0.0.0" || listen == "::" ? "127.0.0.1" : listen;
}

/// "pump = true, setpoint = 42.5" for an event: the first few, then how many more.
std::string listForEvent(const std::vector<std::string>& items, size_t most = 5) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < most; i++) out += (i ? ", " : "") + items[i];
    if (items.size() > most) out += std::format(" and {} more", items.size() - most);
    return out;
}

/// The newest change a user or a token made: {t_ms, message, by}, or null.
json lastChangeJson() {
    auto r = log::lastChange();
    if (!r || !r->by) return nullptr;
    return {{"t_ms", r->timeMs}, {"message", r->message}, {"by", {{"kind", r->by->kind}, {"name", r->by->name}}}};
}

/// Variables of `image` that kept their values in a load that reset `reset` (an online change).
size_t keptCount(const vm::Image& image, const std::vector<std::string>& reset) {
    std::set<std::string> started(reset.begin(), reset.end());
    size_t kept = 0;
    for (const vm::VarEntry& v : image.vars) {
        if (!(v.flags & (vm::kVarExternal | vm::kVarConstant)) && v.path.find("__") == std::string::npos && !started.count(v.path)) kept++;
    }
    return kept;
}

/// Forced values: a forced tag counts once (a 32-bit tag forces two registers), a forced address
/// that no tag covers counts on its own.
size_t forceCount(const ProcessImage& image) {
    std::vector<TagDef> tags = image.tags();
    std::set<std::string> forcedTags;
    size_t untagged = 0;
    for (const ForceEntry& f : image.forces()) {
        auto owner = std::find_if(tags.begin(), tags.end(), [&](const TagDef& t) {
            return t.area == f.area && f.address >= t.address && f.address < t.address + tagWidth(t.type);
        });
        if (owner != tags.end()) forcedTags.insert(owner->name);
        else untagged++;
    }
    return forcedTags.size() + untagged;
}

}  // namespace

std::string numberLiteral(const json& value) {
    if (value.is_number_integer()) return value.dump();
    double d = value.get<double>();
    if (std::trunc(d) == d && std::abs(d) < 1e15) return std::format("{:.1f}", d);  // 42.0
    std::string text = std::format("{}", d);  // shortest form that reads back as the same double
    size_t e = text.find_first_of("eE");
    std::string mantissa = text.substr(0, e);
    if (mantissa.find('.') == std::string::npos) mantissa += ".0";
    return e == std::string::npos ? mantissa : mantissa + "E" + text.substr(e + 1);
}

json tagValueJson(TagType type, double value) {
    if (type == TagType::Bool) return value != 0.0;
    if (type == TagType::Float32) return value;
    return static_cast<int64_t>(value);
}

json liveValueJson(TagType type, double value) {
    if (type == TagType::Float32 && std::isnan(value)) return "NaN";
    if (type == TagType::Float32 && std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    return tagValueJson(type, value);
}

log::Author authorOf(const Principal* by) {
    if (!by) return log::Author::system();
    // A fleet says who asked it; when that is the token itself (a fleet with the same admin token as the device,
    // asked by nobody in particular), "x via x" says nothing twice
    if (!by->onBehalfOf.empty() && by->onBehalfOf != by->name) {
        return {by->token ? "token" : "user", std::format("{} via {}", by->onBehalfOf, by->name)};
    }
    return {by->token ? "token" : "user", by->name};
}

std::string sourceHash(std::string_view source) { return crypto::toHex(crypto::sha256(source)); }

double jsonToValue(const json& value, const std::string& what) {
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_number()) return value.get<double>();
    throw invalid(std::format("{}: the value must be a number or true/false", what));
}

ApiService::ApiService(Engine* engine, Auth& auth) : engine_(engine), auth_(auth) {}

void ApiService::setWebEndpoint(std::string listen, int port) {
    webListen_ = std::move(listen);
    webPort_ = port;
    if (engine_) engine_->setWebPortInUse(port);  // Modbus and OPC UA must not be moved onto the panel's port
}

Engine& ApiService::requireEngine() const {
    if (!engine_) throw notFound("a fleet has no PLC of its own; use the PLCs of its devices");
    return *engine_;
}

std::shared_ptr<Instance> ApiService::plc(const std::string& id) const {
    Engine& engine = requireEngine();
    auto instance = engine.find(id);
    if (!instance) {
        throw notFound(std::format("no PLC with id '{}'; this device's PLC is '{}'", id, engine.config().plc.id));
    }
    return instance;
}

// ───────────────────────────── system ─────────────────────────────

json parseJson(std::string_view text) {
    if (!nestingWithin(text, kMaxJsonDepth)) return json(json::value_t::discarded);
    return json::parse(text, nullptr, false);
}

std::string dumpJson(const json& value, int indent) {
    return value.dump(indent, ' ', false, json::error_handler_t::replace);
}

json ApiService::status() const {
    Engine& engine = requireEngine();
    auto instance = engine.instance();
    json warnings = json::array();
    if (instance) {
        for (const auto& w : instance->warnings()) warnings.push_back(w);
    }
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
    return {{"version", kVersion},
            {"mode", "device"},
            {"plc", engine.config().plc.id},
            {"uptime_s", uptime},
            {"data_dir", engine.dataDir().string()},
            {"web", {{"listen", webListen_}, {"port", webPort_}}},
            {"running", instance && instance->running()},
            {"warnings", warnings}};
}

json ApiService::logs(uint64_t since, const std::string& level, const std::string& plcId, size_t limit) const {
    log::Level max = log::Level::Debug;
    if (!level.empty()) {
        auto parsed = log::parseLevel(level);
        if (!parsed) throw invalid(std::format("unknown log level '{}'; use error, warn, info or debug", level));
        max = *parsed;
    }
    json records = json::array();
    for (const auto& r : log::since(since, std::clamp<size_t>(limit, 1, 2000), max, plcId)) {
        records.push_back({{"seq", r.seq}, {"time", r.timeMs}, {"level", log::levelName(r.level)},
                           {"component", r.component}, {"plc", r.plc}, {"message", r.message}});
        if (!r.event.empty()) records.back()["event"] = r.event;
        if (r.by) records.back()["by"] = {{"kind", r.by->kind}, {"name", r.by->name}};
    }
    return {{"records", records}, {"last_seq", log::lastSeq()}, {"started_ms", startedMs_}};
}

json ApiService::settings() const {
    Config config = requireEngine().config();
    return {{"web", {{"listen", config.web.listen}, {"port", config.web.port}}},
            {"log", {{"level", config.logLevel}}}};
}

json ApiService::updateSettings(const json& body, const Principal* by) {
    requireObject(body);
    Engine& engine = requireEngine();
    Config config = engine.config();
    WebConfig web = config.web;
    std::string level = config.logLevel;
    if (body.contains("web")) {
        const json& w = requireObject(body["web"], "web");
        if (w.contains("listen")) {
            if (!w["listen"].is_string()) throw invalid("web.listen must be a string");
            web.listen = w["listen"].get<std::string>();
        }
        if (w.contains("port")) {
            if (!w["port"].is_number_integer() || w["port"].get<int64_t>() < 1 || w["port"].get<int64_t>() > 65535) {
                throw invalid("web.port must be a whole number, 1-65535");
            }
            web.port = w["port"].get<int>();
        }
    }
    if (body.contains("log")) {
        const json& l = requireObject(body["log"], "log");
        if (l.contains("level")) {
            if (!l["level"].is_string()) throw invalid("log.level must be a string");
            level = l["level"].get<std::string>();
        }
    }
    if (auto ok = engine.updateSettings(web, level); !ok) throw invalid(ok.error());
    std::vector<std::string> changed;
    if (web.listen != config.web.listen) changed.push_back("web listen " + web.listen);
    if (web.port != config.web.port) changed.push_back(std::format("web port {}", web.port));
    if (level != config.logLevel) changed.push_back("log level " + level);
    if (!changed.empty()) log::event(log::Level::Info, "web", config.plc.id, "config", authorOf(by), "settings changed: " + listForEvent(changed));
    json out = settings();
    out["restart_required"] = web != config.web;
    return out;
}

json ApiService::examples() const {
    json out = json::array();
    for (const Example& e : simpleplc::examples()) {
        json tags = json::array();
        for (const auto& t : e.tags) tags.push_back(toJson(t));
        json tasks = json::array();
        for (const auto& t : e.tasks) tasks.push_back(toJson(t));
        json links = json::array();
        for (const auto& l : e.links) links.push_back(toJson(l));
        out.push_back({{"name", e.name}, {"title", e.title}, {"description", e.description},
                       {"tasks", tasks}, {"tags", tags}, {"links", links}});
    }
    return out;
}

json ApiService::example(const std::string& name) const {
    auto e = findExample(name);
    if (!e) throw notFound(std::format("no example named '{}'", name));
    json tags = json::array();
    for (const auto& t : e->tags) tags.push_back(toJson(t));
    json tasks = json::array();
    for (const auto& t : e->tasks) tasks.push_back(toJson(t));
    json links = json::array();
    for (const auto& l : e->links) links.push_back(toJson(l));
    return {{"name", e->name}, {"title", e->title}, {"description", e->description},
            {"tasks", tasks},  {"tags", tags},       {"links", links}, {"programs", e->programs}};
}

std::string ApiService::languageReference() const {
    const embedded::File* doc = embedded::find("docs/structured-text.md");
    return doc ? std::string(doc->text()) : std::string();
}

std::string ApiService::complianceStatement() const {
    const embedded::File* doc = embedded::find("docs/st-compliance.md");
    return doc ? std::string(doc->text()) : std::string();
}

// ───────────────────────────── PLCs ─────────────────────────────

json ApiService::plcSummary(const Instance& instance) const {
    PlcConfig config = instance.config();
    ServerStatus mb = instance.modbusStatus();
    ServerStatus ua = instance.opcuaStatus();
    auto mbStats = instance.modbusStats();
    auto uaStats = instance.opcuaStats();

    json tasks = json::array();
    for (const TaskStatus& t : instance.tasks()) tasks.push_back(taskJson(t));

    json modbus = {{"enabled", mb.enabled},
                   {"listening", mb.listening},
                   {"listen", config.modbus.listen},
                   {"configured_port", mb.configuredPort},
                   {"port", mb.port},
                   {"error", mb.error.empty() ? json(nullptr) : json(mb.error)},
                   {"connections", mbStats.connections.size()},
                   {"total_connections", mbStats.totalConnections},
                   {"rejected_connections", mbStats.rejectedConnections},
                   {"requests", mbStats.requests},
                   {"last_request_ago_ms", mbStats.lastRequestAgoMs < 0 ? json(nullptr) : json(mbStats.lastRequestAgoMs)},
                   {"exceptions", mbStats.exceptions}};
    json clients = json::array();
    for (const auto& c : mbStats.connections) {
        clients.push_back({{"peer", c.peer}, {"requests", c.requests}, {"connected_ms", c.connectedMs}});
    }
    modbus["clients"] = clients;  // this PLC as Modbus TCP server: who polls it
    json links = json::array();     // this PLC as Modbus TCP client: its links to other devices
    for (const auto& l : instance.modbusLinks()) {
        links.push_back({{"name", l.name},
                         {"host", l.host},
                         {"port", l.port},
                         {"unit", l.unit},
                         {"connected", l.connected},
                         {"ok", l.ok},
                         {"cycles", l.cycles},
                         {"requests", l.requests},
                         {"errors", l.errors},
                         {"exceeded", l.exceeded},
                         {"last_error", l.lastError.empty() ? json(nullptr) : json(l.lastError)},
                         {"last_request_ago_ms", l.lastRequestAgoMs < 0 ? json(nullptr) : json(l.lastRequestAgoMs)},
                         {"last_success_ago_ms", l.lastSuccessAgoMs < 0 ? json(nullptr) : json(l.lastSuccessAgoMs)},
                         {"identity", l.identity ? json{{"vendor", l.identity->vendor}, {"product", l.identity->product},
                                                        {"revision", l.identity->revision}}
                                                 : json(nullptr)}});
    }
    modbus["links"] = links;

    json opcua = {{"enabled", ua.enabled},
                  {"listening", ua.listening},
                  {"listen", config.opcua.listen},
                  {"configured_port", ua.configuredPort},
                  {"port", ua.port},
                  {"error", ua.error.empty() ? json(nullptr) : json(ua.error)},
                  {"sessions", uaStats.sessions},
                  {"variables", uaStats.variables},
                  {"url", ua.listening ? json(std::format("opc.tcp://{}:{}", hostFor(config.opcua.listen), ua.port))
                                       : json(nullptr)}};

    // State and fault as one snapshot: the supervisor records a fault and stops the PLC in one step, so
    // read until the fault did not change around the read of the state (never "stopped" without the fault
    // that stopped it)
    std::optional<runtime::FaultInfo> fault = instance.fault();
    bool running = instance.running();
    for (int attempt = 0; attempt < 5; attempt++) {
        auto again = instance.fault();
        bool same = again.has_value() == fault.has_value() &&
                    (!again || (again->atMs == fault->atMs && again->cycle == fault->cycle && again->code == fault->code));
        if (same) break;
        fault = std::move(again);
        running = instance.running();
    }

    std::vector<std::string> warnings = instance.warnings();
    size_t forced = 0;
    if (auto image = instance.image()) forced = forceCount(*image);
    if (forced > 0) {
        // Forced outputs keep their values where substitute values would otherwise be written
        warnings.push_back(forced == 1 ? std::string("1 value is forced: it keeps its forced value in STOP and FAULT")
                                       : std::format("{} values are forced: they keep their forced values in STOP and FAULT", forced));
    }
    for (const json& l : links) {
        if (!l["ok"].get<bool>() && l["last_error"].is_string()) {
            warnings.push_back(std::format("link '{}' to {}:{} is down: {}", l["name"].get<std::string>(),
                                           l["host"].get<std::string>(), l["port"].get<int>(), l["last_error"].get<std::string>()));
        }
    }
    return {{"id", config.id},
            {"name", config.name},
            {"description", config.description},
            {"running", running},
            {"state", fault ? "fault" : running ? "running" : "stopped"},
            {"fault", fault ? faultJson(*fault) : json(nullptr)},
            {"forces", forced},
            {"realtime", config.realtime},
            {"image_size", config.imageSize},
            {"tag_count", config.tags.size()},
            {"modbus", modbus},
            {"opcua", opcua},
            {"tasks", tasks},
            {"warnings", warnings},
            {"last_change", lastChangeJson()}};
}

json ApiService::listPlcs() const {
    json out = json::array();
    if (!engine_) return out;
    if (auto instance = engine_->instance()) {
        json summary = plcSummary(*instance);
        summary["config_version"] = engine_->configVersion();
        out.push_back(summary);
    }
    return out;
}

json ApiService::getPlc(const std::string& id) const {
    auto instance = plc(id);
    // The configuration and its version from one read: a change sent back with this version applies to
    // exactly this configuration
    Config config = requireEngine().config();
    json j = plcSummary(*instance);
    j["config_version"] = config.configVersion;
    j["config"] = toJson(config.plc);
    return j;
}

std::string ApiService::changeConfig(const std::string& id, const std::function<PlcConfig(const PlcConfig&)>& edit,
                                     const ChangeOptions& options, ReconfigureReport* report) {
    ReconfigureReport local;
    ReconfigureReport& rep = report ? *report : local;
    plc(id);  // 404 for another id
    Engine& engine = requireEngine();
    std::string next;
    auto changed = engine.change(
        [&](const PlcConfig& current) {
            if (current.id != id) throw notFound(std::format("no PLC with id '{}'; this device's PLC is '{}'", id, current.id));
            PlcConfig result = edit(current);
            next = result.id;
            return result;
        },
        options.ifVersion, {options.force, options.create}, &rep);
    if (!changed) {
        if (changed.error().stale) {
            throw ApiError(412, "config_changed", "the configuration changed since you loaded it",
                           {{"config_version", engine.configVersion()}, {"problems", changed.error().problems}});
        }
        if (!rep.taskProblems.empty()) {
            throw ApiError(422, "invalid", changed.error().problems.front(),
                           {{"problems", changed.error().problems}, {"programs", programProblemsJson(rep.taskProblems)}});
        }
        if (!rep.tagBreaks.empty()) {
            // The running program would no longer compile, and the PLC would fault
            throw ApiError(409, "tag_in_use",
                           std::format("{}; change the program first, or force the change (the PLC then faults)",
                                       changed.error().problems.front()),
                           {{"programs", programProblemsJson(rep.tagBreaks)}});
        }
        throw invalid(changed.error().problems);
    }
    return next;
}

json ApiService::setupPlc(const std::string& id, const json& body, const ChangeOptions& options, const Principal* by) {
    requireObject(body);
    plc(id);
    Engine& engine = requireEngine();
    // Read, build and compile outside the engine's lock (compiling takes a while); the setup then applies
    // only if nothing changed the configuration meanwhile, else it starts over from the new one
    for (int attempt = 0;; attempt++) {
        Config now = engine.config();
        if (now.plc.id != id) plc(id);  // renamed meanwhile: 404
        try {
            json out = setupFrom(now.plc, body, options.ifVersion.value_or(now.configVersion), by);
            std::string example = body.contains("example") && body["example"].is_string() ? body["example"].get<std::string>() : "";
            log::event(log::Level::Info, "plc", out.value("id", id), "config", authorOf(by),
                       example.empty() || example == "none" ? std::string("set up from scratch") : std::format("set up from example '{}'", example));
            return out;
        } catch (const ApiError& e) {
            if (e.code != "config_changed" || options.ifVersion || attempt >= 3) throw;
        }
    }
}

json ApiService::setupFrom(const PlcConfig& current, const json& body, uint64_t version, const Principal* by) {
    // From scratch, except what other devices rely on: the id and the network ports
    json cfg = {{"id", current.id}};
    json now = toJson(current);
    for (const char* key : {"modbus", "opcua", "device"}) cfg[key] = now[key];
    for (const auto& [key, value] : body.items()) {
        if (key == "example" || key == "program" || key == "programs" || key == "run" || key == "cycle_ms") continue;
        if (value.is_object() && cfg.contains(key) && cfg[key].is_object()) cfg[key].merge_patch(value);
        else cfg[key] = value;
    }

    std::map<std::string, std::string> programs;
    if (body.contains("example") && !body["example"].is_string()) throw invalid("example must be the name of an example, or \"none\"");
    std::string exampleName = body.value("example", std::string());
    if (exampleName == "none") exampleName.clear();  // a blank PLC, as on the command line
    if (!exampleName.empty()) {
        auto e = findExample(exampleName);
        if (!e) throw notFound(std::format("no example named '{}'", exampleName));
        if (!cfg.contains("tags")) {
            cfg["tags"] = json::array();
            for (const auto& t : e->tags) cfg["tags"].push_back(toJson(t));
        }
        if (!cfg.contains("tasks")) {
            cfg["tasks"] = json::array();
            for (const auto& t : e->tasks) cfg["tasks"].push_back(toJson(t));
        }
        if (!cfg.contains("links")) {
            cfg["links"] = json::array();
            for (const auto& l : e->links) cfg["links"].push_back(toJson(l));
        }
        if (!cfg.contains("name")) cfg["name"] = e->title;
        if (!cfg.contains("description")) cfg["description"] = e->description;
        programs = e->programs;
    }
    if (!cfg.contains("name")) cfg["name"] = cfg["id"];

    auto parsed = plcFromJson(cfg, "plc");
    if (!parsed) throw invalid(parsed.error());
    PlcConfig next = *parsed;
    // cycle_ms: the main (first) task's cycle time, also of an example's; a task list gives each its own
    if (body.contains("cycle_ms")) {
        const json& cycle = body["cycle_ms"];
        if (body.contains("tasks")) throw invalid("give cycle_ms in each of \"tasks\", or leave \"tasks\" out to set the main task's");
        if (!cycle.is_number_integer() || cycle.get<int64_t>() < 1 || cycle.get<int64_t>() > 60000) {
            throw invalid("cycle_ms must be a whole number of milliseconds, 1-60000");
        }
        if (next.tasks.empty()) throw invalid("cycle_ms needs a task; this setup has none");
        next.tasks.front().cycleMs = cycle.get<int>();
    }

    if (body.contains("programs")) {
        const json& given = requireObject(body["programs"], "programs");
        for (const auto& [name, source] : given.items()) {
            if (!source.is_string()) throw invalid(std::format("programs.{} must be Structured Text source", name));
            programs[name] = source.get<std::string>();
        }
    }
    if (body.contains("program")) {
        if (!body["program"].is_string()) throw invalid("program must be Structured Text source");
        if (next.tasks.empty()) throw invalid("program needs a task to run it; leave out \"tasks\" or add one");
        programs[next.tasks.front().program] = body["program"].get<std::string>();
    }
    // "none": blank, as on the command line: a task without a given program gets the template, not the old one
    if (body.value("example", std::string()) == "none") {
        for (const TaskConfig& task : next.tasks) programs.emplace(task.program, Engine::templateProgram());
    }
    for (const auto& [name, source] : programs) {
        if (!runtime::ProgramStore::isValidName(name)) throw invalid(std::format("program name '{}' is invalid (letters, digits, _ and -)", name));
    }
    // Compile every task's program with the new tags and files first: a program that does not
    // compile must not replace a working PLC
    for (const TaskConfig& task : next.tasks) {
        auto it = programs.find(task.program);
        if (it == programs.end()) continue;
        std::vector<st::SourceFile> files{{task.program + ".st", it->second}};
        for (const auto& [name, source] : programs) {
            if (name != task.program) files.push_back({name + ".st", source});
        }
        st::CompileResult r = st::compile(files, next.tags);
        if (!r.ok) {
            auto [line, error] = firstError(r);
            throw ApiError(422, "invalid", std::format("program '{}' line {}: {}", task.program, line, error),
                           {{"program", task.program}, {"line", line}, {"error", error}, {"diagnostics", diagnosticsJson(r)}});
        }
    }
    bool run = true;
    if (body.contains("run")) {
        if (!body["run"].is_boolean()) throw invalid("run must be true or false");
        run = body["run"].get<bool>();
    }

    if (auto ok = requireEngine().setup(next, programs, run, version, authorOf(by)); !ok) {
        if (ok.error().stale) {
            throw ApiError(412, "config_changed", "the configuration changed since you loaded it",
                           {{"config_version", requireEngine().configVersion()}, {"problems", ok.error().problems}});
        }
        throw invalid(ok.error().problems);
    }
    // A program can still fail while loading (an error in its top level or init()): the PLC is set up,
    // but say which tasks have no program running
    json out = getPlc(next.id);
    json results = json::array();
    auto errors = plc(next.id)->programErrors();
    for (const TaskConfig& task : next.tasks) {
        auto it = errors.find(task.name);
        json r = {{"task", task.name}, {"ok", it == errors.end()}};
        if (it != errors.end()) r["error"] = it->second;
        results.push_back(r);
    }
    out["loaded"] = errors.empty();
    out["results"] = results;
    return out;
}

json ApiService::updatePlc(const std::string& id, const json& patch, const ChangeOptions& options, const Principal* by) {
    requireObject(patch);
    json changes = patch;
    ChangeOptions opts = options;
    opts.create = takeFlag(changes, "create") || opts.create;
    ReconfigureReport report;
    std::string next = changeConfig(id, [&](const PlcConfig& current) {
        json merged = toJson(current);
        merged.merge_patch(changes);
        auto parsed = plcFromJson(merged, "plc");
        if (!parsed) throw invalid(parsed.error());
        return *parsed;
    }, opts, &report);
    std::vector<std::string> keys;
    for (const auto& [key, unused] : changes.items()) keys.push_back(key);
    log::event(log::Level::Info, "plc", next, "config", authorOf(by), "configuration changed: " + listForEvent(keys, 8));
    json out = getPlc(next);
    out["restarted_tasks"] = report.restartedTasks;
    return out;
}

json ApiService::device(const std::string& id) const {
    plc(id);
    return collectDeviceInfo(requireEngine().dataDir(), started_);
}

json ApiService::runPlc(const std::string& id, const Principal* by) {
    plc(id)->run(authorOf(by));
    return getPlc(id);
}

json ApiService::stopPlc(const std::string& id, const Principal* by) {
    plc(id)->halt(authorOf(by));
    return getPlc(id);
}

json ApiService::restartPlc(const std::string& id, const Principal* by) {
    auto instance = plc(id);
    log::event(log::Level::Info, "plc", id, "mode", authorOf(by),
               "cold restart: tags start from 0, every variable from its initial value, retentive memory cleared");
    instance->close();
    instance->clearRetained();
    instance->open(true, authorOf(by));  // a restart someone asked for ends in RUN, also after the next power-up
    return getPlc(id);
}

// ───────────────────────────── tags and values ─────────────────────────────

namespace {

/// Tag name -> what the tags list adds about who writes it: "writer" for a program (one writer per tag)
/// or a link that reads it from a device, "sent_by" for a tag a link writes to a device, "status_of" for a
/// link's status tag.
json tagWriters(const Instance& instance) {
    json out = json::object();
    PlcConfig config = instance.config();
    for (const TaskStatus& t : instance.tasks()) {
        auto task = instance.task(t.config.name);
        auto image = task ? task->latestImage() : nullptr;  // a download waiting for its cycle boundary writes them next
        if (!image) continue;
        for (const vm::Binding& b : image->bindings) {
            if (!b.readOnly && !out[b.tag].contains("writer")) {
                out[b.tag]["writer"] = {{"kind", "program"}, {"task", t.config.name}, {"program", t.config.program}};
            }
        }
    }
    for (const LinkConfig& link : config.links) {
        for (const modbus::LinkMapping& m : link.read) {
            if (!out[m.tag].contains("writer")) {
                out[m.tag]["writer"] = {{"kind", "link"}, {"link", link.name}, {"area", areaName(m.area)}, {"address", m.address}, {"unit", link.unit}};
            }
        }
        for (const modbus::LinkMapping& m : link.write) {
            if (!out[m.tag].contains("sent_by")) out[m.tag]["sent_by"] = {{"link", link.name}, {"area", areaName(m.area)}, {"address", m.address}};
        }
        if (!link.statusTag.empty()) {
            out[link.statusTag]["status_of"] = {{"link", link.name}};
            if (!out[link.statusTag].contains("writer")) {
                out[link.statusTag]["writer"] = {{"kind", "link"}, {"link", link.name}, {"area", nullptr}, {"address", nullptr}, {"unit", link.unit}};
            }
        }
    }
    return out;
}

}  // namespace

json ApiService::tags(const std::string& id) const {
    auto instance = plc(id);
    auto image = instance->image();
    json out = json::array();
    if (!image) return out;
    json writers = tagWriters(*instance);
    for (const TagDef& tag : image->tags()) {
        json j = toJson(tag);
        j["value"] = tagValueJson(tag.type, image->readTag(tag));
        j["forced"] = image->isForced(tag.area, tag.address);
        j["origin"] = originName(image->lastOrigin(tag.area, tag.address));
        j["writable"] = isClientWritable(tag.area);
        j["writer"] = json{{"kind", "clients"}};  // nothing in the PLC writes it: Modbus, OPC UA and API clients do
        if (writers.contains(tag.name) && writers[tag.name].is_object()) j.update(writers[tag.name]);
        out.push_back(j);
    }
    return out;
}

json ApiService::setTags(const std::string& id, const json& tagsJson, const ChangeOptions& options, const Principal* by) {
    auto parsed = tagsFromJson(tagsJson, "tags");
    if (!parsed) throw invalid(parsed.error());
    std::string next = changeConfig(id, [&](const PlcConfig& current) {
        PlcConfig changed = current;
        changed.tags = *parsed;
        return changed;
    }, options);
    log::event(log::Level::Info, "plc", next, "config", authorOf(by),
               std::format("tag table replaced: {} tag{}", parsed->size(), parsed->size() == 1 ? "" : "s"));
    return tags(next);
}

json ApiService::addTag(const std::string& id, const json& tagJson, const ChangeOptions& options, const Principal* by) {
    auto parsed = tagFromJson(tagJson, "tag");
    if (!parsed) throw invalid(parsed.error());
    changeConfig(id, [&](const PlcConfig& current) {
        PlcConfig next = current;
        for (const TagDef& t : next.tags) {
            if (st::toUpper(t.name) == st::toUpper(parsed->name)) {
                throw ApiError(409, "conflict", t.name == parsed->name ? std::format("tag '{}' already exists", t.name)
                                                                       : std::format("tag '{}' already exists, and case does not "
                                                                                     "matter in names", t.name));
            }
        }
        next.tags.push_back(*parsed);
        return next;
    }, options);
    log::event(log::Level::Info, "plc", id, "config", authorOf(by),
               std::format("tag '{}' added ({} {})", parsed->name, areaName(parsed->area), parsed->address));
    return toJson(*parsed);
}

json ApiService::updateTag(const std::string& id, const std::string& name, const json& patch, const ChangeOptions& options,
                           const Principal* by) {
    requireObject(patch);
    TagDef updated;
    changeConfig(id, [&](const PlcConfig& current) {
        PlcConfig next = current;
        auto it = std::find_if(next.tags.begin(), next.tags.end(), [&](const TagDef& t) { return t.name == name; });
        if (it == next.tags.end()) throw notFound(std::format("no tag named '{}'", name));
        json merged = toJson(*it);
        merged.merge_patch(patch);
        if (patch.contains("area") && !patch.contains("type")) merged.erase("type");  // default type for the new area
        auto parsed = tagFromJson(merged, "tag");
        if (!parsed) throw invalid(parsed.error());
        *it = updated = *parsed;
        return next;
    }, options);
    std::vector<std::string> keys;
    for (const auto& [key, unused] : patch.items()) keys.push_back(key);
    log::event(log::Level::Info, "plc", id, "config", authorOf(by), std::format("tag '{}' changed: {}", name, listForEvent(keys)));
    return toJson(updated);
}

void ApiService::deleteTag(const std::string& id, const std::string& name, const ChangeOptions& options, const Principal* by) {
    changeConfig(id, [&](const PlcConfig& current) {
        PlcConfig next = current;
        if (std::erase_if(next.tags, [&](const TagDef& t) { return t.name == name; }) == 0) {
            throw notFound(std::format("no tag named '{}'", name));
        }
        return next;
    }, options);
    log::event(log::Level::Info, "plc", id, "config", authorOf(by), std::format("tag '{}' deleted", name));
}

json ApiService::values(const std::string& id, const std::vector<std::string>& names) const {
    auto image = plc(id)->image();
    json out = json::object();
    if (names.empty()) {
        for (const TagDef& tag : image->tags()) out[tag.name] = tagValueJson(tag.type, image->readTag(tag));
        return out;
    }
    std::vector<std::string> unknown;
    for (const std::string& name : names) {
        auto value = image->readTag(name);
        if (value) {
            out[name] = tagValueJson(value->type, value->value);
        } else {
            unknown.push_back(name);
        }
    }
    if (!unknown.empty()) {
        throw notFound(std::format("unknown tag{} {}", unknown.size() > 1 ? "s" : "", dumpJson(json(unknown))));
    }
    return out;
}

json ApiService::writeValues(const std::string& id, const json& body, const Principal* by) {
    requireObject(body, "the values");
    auto image = plc(id)->image();
    std::vector<std::pair<std::string, double>> writes;
    std::vector<std::string> names;
    for (const auto& [name, value] : body.items()) {
        writes.emplace_back(name, jsonToValue(value, name));
        names.push_back(name);
    }
    if (writes.empty()) throw invalid("give at least one tag and value, e.g. {\"pump\": true}");
    if (auto ok = image->writeTags(writes, Origin::Api); !ok) {
        if (ok.error().starts_with("unknown tag")) throw notFound(ok.error());
        throw invalid(ok.error());
    }
    std::vector<std::string> written;
    for (const auto& [name, value] : body.items()) written.push_back(std::format("{} = {}", name, dumpJson(value)));
    log::event(log::Level::Info, "plc", id, "write", authorOf(by), "wrote " + listForEvent(written));
    return values(id, names);
}

json ApiService::readArea(const std::string& id, const std::string& areaName, int start, int count) const {
    Area area = areaArg(areaName);
    auto image = plc(id)->image();
    if (count < 1 || count > 2000) throw invalid("count must be 1-2000");
    if (start < 0) throw invalid(std::format("start must be an address from 0 to {}", image->size() - 1));
    if (!image->contains(static_cast<uint64_t>(start), static_cast<uint64_t>(count))) {
        throw invalid(std::format("addresses {}-{} are outside the image (0-{})", start, int64_t{start} + count - 1, image->size() - 1));
    }
    std::vector<uint16_t> raw(static_cast<size_t>(count));
    image->read(area, static_cast<uint32_t>(start), raw);
    json values = json::array();
    for (uint16_t v : raw) {
        if (isBitArea(area)) {
            values.push_back(v != 0);
        } else {
            values.push_back(v);
        }
    }
    return {{"area", simpleplc::areaName(area)}, {"start", start}, {"values", values}};
}

json ApiService::writeArea(const std::string& id, const std::string& areaName, const json& body, const Principal* by) {
    requireObject(body);
    Area area = areaArg(areaName);
    auto image = plc(id)->image();
    if (!body.contains("start") || !body["start"].is_number_integer() || body["start"].get<int64_t>() < 0 ||
        body["start"].get<int64_t>() >= static_cast<int64_t>(image->size())) {
        throw invalid(std::format("give \"start\": the first address, 0-{}", image->size() - 1));
    }
    int start = body["start"].get<int>();
    if (!body.contains("values") || !body["values"].is_array() || body["values"].empty()) {
        throw invalid("give {\"start\": address, \"values\": [...]}");
    }
    std::vector<uint16_t> raw;
    for (const json& v : body["values"]) {
        double x = jsonToValue(v, "values");
        if (isBitArea(area)) {
            if (x != 0.0 && x != 1.0) throw invalid("bit values must be true/false or 0/1");
        } else if (x != std::trunc(x) || x < -32768 || x > 65535) {
            throw invalid(std::format("register value {} is not a whole number from -32768 to 65535", x));
        }
        raw.push_back(static_cast<uint16_t>(static_cast<int32_t>(x)));
    }
    if (start < 0 || !image->contains(static_cast<uint64_t>(start), raw.size())) {
        throw invalid(std::format("addresses from {} ({} values) are outside the image (0-{})", start, raw.size(), image->size() - 1));
    }
    // As over Modbus: a float32 tag never becomes NaN or infinite from outside
    if (auto ok = image->writeChecked(area, static_cast<uint32_t>(start), raw, Origin::Api); !ok) throw invalid(ok.error());
    log::event(log::Level::Info, "plc", id, "write", authorOf(by),
               raw.size() == 1 ? std::format("wrote {} {} = {}", areaName, start, raw[0])
                               : std::format("wrote {} {}-{} ({} values)", areaName, start, start + static_cast<int>(raw.size()) - 1, raw.size()));
    return readArea(id, areaName, start, static_cast<int>(raw.size()));
}

json ApiService::forces(const std::string& id) const {
    auto image = plc(id)->image();
    auto tagList = image->tags();
    json out = json::array();
    for (const ForceEntry& f : image->forces()) {
        json j = {{"area", areaName(f.area)}, {"address", f.address}, {"value", f.value}};
        for (const TagDef& t : tagList) {
            if (t.area == f.area && f.address >= t.address && f.address < t.address + tagWidth(t.type)) {
                j["tag"] = t.name;
                break;
            }
        }
        out.push_back(j);
    }
    return out;
}

json ApiService::force(const std::string& id, const json& body, const Principal* by) {
    requireObject(body, "the values");
    if (body.empty()) throw invalid("give at least one tag and the value to force it to, e.g. {\"motor\": false}");
    auto image = plc(id)->image();
    // {"tag": value, ...}: force tags to values. All are checked first, so a bad entry forces nothing.
    struct Forced {
        TagDef tag;
        uint16_t words[2];
    };
    std::vector<Forced> forced;
    std::string summary;
    for (const auto& [name, value] : body.items()) {
        auto tag = image->findTag(name);
        if (!tag) throw notFound(std::format("unknown tag '{}'", name));
        Forced f{*tag, {0, 0}};
        if (auto error = encodeTagValue(tag->type, jsonToValue(value, name), f.words)) {
            throw invalid(std::format("tag '{}': {}", name, *error));
        }
        forced.push_back(f);
        summary += std::format("{}{} = {}", summary.empty() ? "" : ", ", name, dumpJson(value));
    }
    for (const Forced& f : forced) {
        for (uint32_t i = 0; i < tagWidth(f.tag.type); i++) image->force(f.tag.area, f.tag.address + i, f.words[i]);
    }
    // Forcing overrides the program and every client: always worth a line in the log
    if (!forced.empty()) log::event(log::Level::Info, "plc", id, "force", authorOf(by), "forced " + summary);
    return forces(id);
}

json ApiService::release(const std::string& id, const json& body, const Principal* by) {
    auto image = plc(id)->image();
    // {"tags": [...]} releases those tags; anything else releases all
    log::Scope log("plc", id);
    if (body.is_object() && body.contains("tags")) {
        if (!body["tags"].is_array()) throw invalid("tags must be an array of tag names");
        std::vector<TagDef> tags;
        for (const json& name : body["tags"]) {
            if (!name.is_string()) throw invalid("tags must be an array of tag names");
            auto tag = image->findTag(name.get<std::string>());
            if (!tag) throw notFound(std::format("unknown tag '{}'", name.get<std::string>()));
            tags.push_back(*tag);
        }
        std::string released;
        for (const TagDef& tag : tags) {
            if (!image->isForced(tag.area, tag.address)) continue;
            for (uint32_t i = 0; i < tagWidth(tag.type); i++) image->release(tag.area, tag.address + i);
            released += std::format("{}{}", released.empty() ? "" : ", ", tag.name);
        }
        if (!released.empty()) log.event(log::Level::Info, "release", authorOf(by), "released {}", released);
    } else if (!image->forces().empty()) {
        image->releaseAll();
        log.event(log::Level::Info, "release", authorOf(by), "released all forces");
    }
    return forces(id);
}

std::optional<json> ApiService::liveValues(const std::string& id, LiveState& state) const {
    auto image = plc(id)->image();
    // A cold restart or a new image size gives the PLC a new image, whose versions start again and whose size
    // may be smaller: the slots below are only valid for the image they were made for
    bool newImage = image != state.image;
    // The versions before the values: a write that comes between them is seen by the next poll
    uint64_t version = image->version();
    uint64_t tagsVersion = image->tagsVersion();
    uint64_t forcesVersion = image->forcesVersion();
    if (!state.first && !newImage && version == state.version && tagsVersion == state.tagsVersion && forcesVersion == state.forcesVersion) {
        return std::nullopt;
    }
    bool full = state.first || newImage || tagsVersion != state.tagsVersion;
    if (full) {
        // The table is validated against the image's size when it is set, so every slot is inside the image
        state.tags = image->tags();
        state.slots.clear();
        size_t words = 0;
        for (const TagDef& t : state.tags) {
            state.slots.push_back(Slot{t.area, t.address, static_cast<uint8_t>(tagWidth(t.type))});
            words += tagWidth(t.type);
        }
        state.words.assign(words, 0);
        state.nextWords.assign(words, 0);
        state.origins.assign(state.tags.size(), Origin::None);
        state.nextOrigins.assign(state.tags.size(), Origin::None);
        state.forced.assign(state.tags.size(), 0);
        state.nextForced.assign(state.tags.size(), 0);
    }
    image->readSlots(state.slots, state.nextWords.data(), state.nextOrigins.data());
    if (full || forcesVersion != state.forcesVersion) {
        // A tag is forced when any of its entries is (the API forces all of them; a raw force may cover one)
        std::fill(state.nextForced.begin(), state.nextForced.end(), uint8_t{0});
        for (const ForceEntry& f : image->forces()) {
            for (size_t i = 0; i < state.tags.size(); i++) {
                const TagDef& t = state.tags[i];
                if (t.area == f.area && f.address >= t.address && f.address < t.address + tagWidth(t.type)) state.nextForced[i] = 1;
            }
        }
    } else {
        state.nextForced = state.forced;
    }

    json values = json::object();
    json origins = json::object();
    json forced = json::array();
    size_t off = 0;
    for (size_t i = 0; i < state.tags.size(); i++) {
        const TagDef& t = state.tags[i];
        uint32_t width = tagWidth(t.type);
        const uint16_t* now = state.nextWords.data() + off;
        bool changed = full || !std::equal(now, now + width, state.words.data() + off) || state.nextOrigins[i] != state.origins[i] ||
                       state.nextForced[i] != state.forced[i];
        if (changed) {
            values[t.name] = liveValueJson(t.type, decodeTagValue(t.type, now));
            origins[t.name] = originName(state.nextOrigins[i]);
        }
        if (state.nextForced[i]) forced.push_back(t.name);
        off += width;
    }
    bool forcesChanged = full || state.nextForced != state.forced;
    std::swap(state.words, state.nextWords);
    std::swap(state.origins, state.nextOrigins);
    std::swap(state.forced, state.nextForced);
    state.image = image;
    state.version = version;
    state.tagsVersion = tagsVersion;
    state.forcesVersion = forcesVersion;
    state.first = false;
    if (!full && values.empty() && !forcesChanged) return std::nullopt;  // e.g. a write to an entry no tag covers
    return json{{"full", full}, {"values", std::move(values)}, {"origins", std::move(origins)}, {"forced", std::move(forced)}};
}

// ───────────────────────────── tasks and programs ─────────────────────────────

json ApiService::tasks(const std::string& id) const {
    json out = json::array();
    for (const TaskStatus& t : plc(id)->tasks()) out.push_back(taskJson(t));
    return out;
}

json ApiService::updateTask(const std::string& id, const std::string& task, const json& patch, const ChangeOptions& options,
                            const Principal* by) {
    requireObject(patch);
    json changes = patch;
    ChangeOptions opts = options;
    opts.create = takeFlag(changes, "create") || opts.create;
    ReconfigureReport report;
    std::string next = changeConfig(id, [&](const PlcConfig& current) {
        auto it = std::find_if(current.tasks.begin(), current.tasks.end(), [&](const TaskConfig& t) { return t.name == task; });
        if (it == current.tasks.end()) throw notFound(std::format("no task named '{}'", task));
        json merged = toJson(*it);
        merged.merge_patch(changes);
        json plcJson = toJson(current);
        plcJson["tasks"][static_cast<size_t>(it - current.tasks.begin())] = merged;
        auto parsed = plcFromJson(plcJson, "plc");
        if (!parsed) throw invalid(parsed.error());
        return *parsed;
    }, opts, &report);
    std::vector<std::string> keys;
    for (const auto& [key, value] : changes.items()) keys.push_back(std::format("{} {}", key, dumpJson(value)));
    log::event(log::Level::Info, "plc", next, "config", authorOf(by), std::format("task '{}' changed: {}", task, listForEvent(keys)));
    return {{"tasks", tasks(next)}, {"restarted_tasks", report.restartedTasks}};
}

json ApiService::variables(const std::string& id, const std::string& task, const std::string& prefix) const {
    auto instance = plc(id);
    std::string name = task;
    if (name.empty()) {
        auto list = instance->tasks();
        if (list.empty()) throw notFound("the PLC has no tasks");
        name = list.front().config.name;
    }
    auto t = instance->task(name);
    if (!t) throw notFound(std::format("no task named '{}'", name));
    runtime::Task::Snapshot snap = t->snapshot();
    if (!t->image()) throw ApiError(409, "conflict", std::format("task '{}' has no program loaded", name));
    if (!snap.image) throw ApiError(503, "busy", std::format("task '{}' did not finish a cycle in time; try again", name));
    const vm::Image* image = snap.image.get();
    const std::vector<uint8_t>* data = snap.data.get();
    std::string want = st::toUpper(prefix);
    json vars = json::array();
    constexpr size_t kMaxVariables = 5000;  // an answer an agent can read
    bool truncated = false;
    for (const vm::VarEntry& v : image->vars) {
        if (!want.empty() && !v.path.starts_with(want)) continue;
        if (v.path.find("__") != std::string::npos) continue;  // standard library internals
        json j = {{"path", v.path}, {"type", v.typeName}};
        if (data && data->size() >= v.offset + v.size) {
            j["value"] = st::variableJson(*image, v, data->data());
            j["text"] = st::variableText(*image, v, data->data());
        }
        json flags = json::array();
        if (v.flags & vm::kVarRetain) flags.push_back("retain");
        if (v.flags & vm::kVarConstant) flags.push_back("constant");
        if (v.flags & vm::kVarExternal) flags.push_back("tag");
        if (v.flags & vm::kVarInput) flags.push_back("input");
        if (v.flags & vm::kVarOutput) flags.push_back("output");
        j["flags"] = flags;
        if (vars.size() >= kMaxVariables) {
            truncated = true;
            break;
        }
        vars.push_back(j);
    }
    json out = {{"task", name}, {"program", image->program}, {"variables", vars}, {"truncated", truncated}};
    if (truncated) {
        out["hint"] = std::format("only the first {} variables are listed: pass a prefix (e.g. {}.MOTOR) to see the others",
                                  kMaxVariables, st::toUpper(image->program));
    }
    return out;
}

json ApiService::setVariables(const std::string& id, const json& body, const Principal* by) {
    requireObject(body);
    auto instance = plc(id);
    std::string name = body.value("task", "");
    if (name.empty()) {
        auto list = instance->tasks();
        if (list.empty()) throw notFound("the PLC has no tasks");
        name = list.front().config.name;
    }
    auto t = instance->task(name);
    if (!t) throw notFound(std::format("no task named '{}'", name));
    auto image = t->image();
    if (!image) throw ApiError(409, "conflict", std::format("task '{}' has no program loaded", name));
    if (!body.contains("values") || !body["values"].is_object()) throw invalid("values must be an object: {\"MAIN.X\": 42}");
    // Check every value first, then write them all
    std::vector<std::pair<std::string, std::vector<uint8_t>>> writes;
    for (const auto& [path, value] : body["values"].items()) {
        std::string up = st::toUpper(path);
        if (up.find('.') == std::string::npos) up = st::toUpper(image->program) + "." + up;
        const vm::VarEntry* var = nullptr;
        for (const vm::VarEntry& v : image->vars) {
            if (v.path == up) var = &v;
        }
        if (!var) throw notFound(std::format("no variable {}", path));
        // Every value is checked before anything is written: all or nothing
        if (var->flags & vm::kVarExternal) throw invalid(std::format("{} is a tag: write the tag instead", path));
        if (var->flags & vm::kVarConstant) throw invalid(std::format("{} is CONSTANT", path));
        if (!value.is_string() && !value.is_number() && !value.is_boolean()) {
            throw invalid(std::format("{}: the value must be a number, true/false or an ST literal", path));
        }
        std::string text = value.is_string() ? value.get<std::string>() : value.is_number() ? numberLiteral(value) : value.dump();
        if (value.is_boolean()) text = value.get<bool>() ? "TRUE" : "FALSE";
        auto bytes = st::literalBytes(*image, *var, text);
        if (!bytes) throw invalid(bytes.error());
        writes.push_back({up, std::move(*bytes)});
    }
    std::vector<std::string> written;
    for (const auto& [path, value] : body["values"].items()) written.push_back(std::format("{} = {}", path, dumpJson(value)));
    for (auto& [path, bytes] : writes) {
        if (auto ok = t->write(path, std::move(bytes)); !ok) throw invalid(ok.error());
    }
    log::event(log::Level::Info, "plc", id, "write", authorOf(by), std::format("wrote {} (task '{}')", listForEvent(written), name));
    return {{"task", name}, {"written", writes.size()}};
}

json ApiService::programs(const std::string& id) const {
    auto instance = plc(id);
    auto config = instance->config();
    json out = json::array();
    for (const auto& p : instance->programs().list()) {
        json usedBy = json::array();
        for (const auto& t : config.tasks) {
            if (runtime::ProgramStore::sameName(t.program, p.name)) usedBy.push_back(t.name);
        }
        out.push_back({{"name", p.name}, {"size", p.size}, {"modified", p.modifiedMs}, {"used_by", usedBy}});
    }
    return out;
}

std::string ApiService::programSource(const std::string& id, const std::string& name) const {
    auto source = plc(id)->programs().read(name);
    if (!source) throw notFound(source.error());
    return *source;
}

std::string ApiService::mainProgram(const std::string& id) const { return plc(id)->mainProgram(); }

json ApiService::saveProgram(const std::string& id, const std::string& name, const std::string& source, bool deploy,
                             const std::optional<std::string>& ifMatch, const Principal* by) {
    auto instance = plc(id);
    SaveOutcome save = instance->saveProgram(name, source, deploy, ifMatch);
    if (save.conflictHash) {
        // Somebody saved another version since the caller read it
        throw ApiError(412, "program_changed", "the program changed since you opened it: someone else saved it",
                       {{"hash", save.conflictHash->empty() ? json(nullptr) : json(*save.conflictHash)}});
    }
    if (!save.saved) throw invalid(save.error);
    logProgramEvent(id, save, deploy, by);
    json out = {{"saved", true}, {"program", save.program}, {"deployed", deploy}, {"hash", sourceHash(source)}};
    if (!deploy) {
        out["loaded"] = nullptr;
        out["results"] = json::array();
        return out;
    }
    const ProgramLoad& own = save.own;
    json results = json::array();
    auto [line, error] = firstError(own.compiled);
    for (const std::string& task : own.tasks) {
        auto refused = own.taskErrors.find(task);
        bool ok = own.ok && refused == own.taskErrors.end();
        json r = {{"task", task}, {"ok", ok}};
        if (!own.compiled.ok) {
            r["error"] = error;
            r["line"] = line;
        } else if (refused != own.taskErrors.end()) {
            r["error"] = refused->second;
        } else if (!own.error.empty()) {
            r["error"] = own.error;
        }
        results.push_back(r);
    }
    // The running programs that use the file as a library: they took it over, or keep their version
    json dependents = json::array();
    for (const ProgramLoad& d : save.dependents) {
        auto [dLine, dError] = firstError(d.compiled);
        std::string dFile;
        for (const st::Diagnostic& diag : d.compiled.diagnostics) {
            if (diag.severity == st::Severity::Error) {
                dFile = diag.loc.file < d.compiled.fileNames.size() ? d.compiled.fileNames[diag.loc.file] : "";
                break;
            }
        }
        for (const std::string& task : d.tasks) {
            auto refused = d.taskErrors.find(task);
            json r = {{"task", task}, {"program", d.program}, {"ok", d.ok}};
            if (!d.compiled.ok) {
                r["error"] = dError;
                r["file"] = dFile;
                r["line"] = dLine;
            } else if (refused != d.taskErrors.end()) {
                r["error"] = refused->second;
            }
            results.push_back(r);
        }
        json dj = {{"program", d.program}, {"tasks", d.tasks}, {"ok", d.ok}};
        if (d.ok) putReset(dj, d.reset);
        else dj["diagnostics"] = diagnosticsJson(d.compiled);
        dependents.push_back(dj);
    }
    out["results"] = results;
    out["loaded"] = save.ok();
    out["diagnostics"] = diagnosticsJson(own.compiled);
    out["dependents"] = dependents;
    if (!own.error.empty()) out["error"] = own.error;
    if (save.library) out["library"] = true;
    if (own.ok && !save.library) {
        out["memory"] = {{"data_bytes", own.compiled.dataBytes}, {"stack_bytes", own.compiled.stackBytes},
                         {"instructions", own.compiled.codeInstructions}};
        putReset(out, own.reset);  // variables that started from their initial value
        if (!own.tasks.empty()) out["replaced"] = own.replaced;  // false: there was no program before, all start fresh
    }
    if (own.tasks.empty()) {
        if (!save.library) out["note"] = "no task runs this program; assign it to a task to run it";
        else if (save.dependents.empty()) out["note"] = "a library: no task runs it, and no running program uses it yet";
        else out["note"] = "a library: no task runs it; the running programs that use it are listed in dependents";
    }
    return out;
}

json ApiService::checkProgram(const std::string& id, const std::string& name, const std::string& source,
                              const json& tags) const {
    if (source.size() > runtime::ProgramStore::kMaxSourceBytes) {
        throw invalid(std::format("the program is larger than {} KB", runtime::ProgramStore::kMaxSourceBytes / 1024));
    }
    std::optional<std::vector<TagDef>> given;
    if (!tags.is_null()) {
        auto parsed = tagsFromJson(tags, "tags");
        if (!parsed) throw invalid(parsed.error());
        given = *parsed;
    }
    auto instance = plc(id);
    st::CompileResult r = instance->checkProgram(name.empty() ? instance->mainProgram() : name, source, given);
    json j = compileJson(r);
    if (r.library) {
        j["library"] = true;  // no PROGRAM and no task runs it: its declarations were checked
        j.erase("memory");
    }
    auto [line, error] = firstError(r);
    if (!r.ok) {
        j["line"] = line;
        j["error"] = error;
    }
    // What a download would do to the running values (only for a program a task runs)
    if (r.ok && r.image && !r.library) {
        if (auto preview = instance->previewOnlineChange(name.empty() ? instance->mainProgram() : name, *r.image)) {
            json change = {{"tasks", preview->tasks}, {"kept", preview->kept}};
            putReset(change, preview->reset);
            j["online_change"] = change;
        }
    }
    return j;
}

json ApiService::compiledProgram(const std::string& id, const std::string& name) const {
    auto instance = plc(id);
    std::string program = name.empty() ? instance->mainProgram() : instance->programs().canonical(name);
    // The version the tasks run; a program no task runs: its last compilation, or the saved file compiled now
    auto r = instance->compiled(program);
    bool deployed = instance->deployedFiles(program).has_value() && r.has_value();
    if (!r) {
        auto source = instance->programs().read(program);
        if (!source) throw notFound(source.error());
        r = instance->checkProgram(program, *source);
    }
    json j = compileJson(*r);
    j["program"] = program;
    j["listing"] = r->listing;
    j["deployed"] = deployed;
    if (r->library) j["library"] = true;
    return j;
}

json ApiService::deployedProgram(const std::string& id, const std::string& name) const {
    auto instance = plc(id);
    auto files = instance->deployedFiles(name);
    if (!files || files->empty()) {
        if (!instance->programs().exists(name)) throw notFound(std::format("program '{}' does not exist", name));
        throw notFound(std::format("program '{}' has not run in a task yet: it has no deployed version", name));
    }
    json list = json::array();
    for (const runtime::StoredFile& f : *files) {
        list.push_back({{"name", f.name}, {"source", f.text}, {"hash", sourceHash(f.text)}});
    }
    auto saved = instance->programs().read(files->front().name);
    // draft: the saved file is not what runs (refused, or saved without loading it)
    return {{"program", files->front().name}, {"files", list}, {"draft", !saved || *saved != files->front().text}};
}

void ApiService::deleteProgram(const std::string& id, const std::string& name) {
    // Refused while a task runs it or a loaded program uses it; its history keeps it either way
    if (auto ok = plc(id)->deleteProgram(name); !ok) {
        throw ApiError(ok.error().first, ok.error().first == 409 ? "conflict" : "not_found", ok.error().second);
    }
}

json ApiService::programHistory(const std::string& id, const std::string& name) const {
    json out = json::array();
    for (const auto& v : plc(id)->programs().history(name)) {
        out.push_back({{"version", v.id}, {"size", v.size}, {"saved", v.savedMs}});
    }
    return out;
}

std::string ApiService::programVersion(const std::string& id, const std::string& name, const std::string& version) const {
    auto source = plc(id)->programs().readVersion(name, version);
    if (!source) throw notFound(source.error());
    return *source;
}

json ApiService::restoreProgram(const std::string& id, const std::string& name, const std::string& version, bool deploy,
                                const Principal* by) {
    std::string source = programVersion(id, name, version);
    return saveProgram(id, name, source, deploy, std::nullopt, by);
}

// ───────────────────────────── users and tokens ─────────────────────────────

json ApiService::users() const {
    json out = json::array();
    for (const auto& u : auth_.users()) out.push_back({{"name", u.name}, {"role", roleName(u.role)}, {"created", u.createdMs}});
    return out;
}

json ApiService::addUser(const json& body, const Principal* by) {
    requireObject(body);
    auto role = parseRole(body.contains("role") && body["role"].is_string() ? body["role"].get<std::string>() : body.contains("role") ? "" : "viewer");
    if (!role) throw invalid("role must be admin or viewer");
    if (!body.contains("name") || !body["name"].is_string() || !body.contains("password") || !body["password"].is_string()) {
        throw invalid("give a name and a password");
    }
    if (auto ok = auth_.addUser(body["name"].get<std::string>(), body["password"].get<std::string>(), *role); !ok) {
        throw invalid(ok.error());
    }
    accessEvent(by, std::format("user '{}' added ({})", body["name"].get<std::string>(), roleName(*role)));
    return users();
}

json ApiService::removeUser(const std::string& name, const Principal& by) {
    if (!by.token && by.name == name) throw invalid("you cannot remove your own account");
    auto removed = auth_.removeUser(name);
    if (!removed) throw invalid(removed.error());
    json revoked = json::array();
    for (const TokenInfo& t : *removed) revoked.push_back({{"id", t.id}, {"name", t.name}});
    accessEvent(&by, removed->empty() ? std::format("user '{}' removed", name)
                                      : std::format("user '{}' removed with the {} token{} they created", name, removed->size(),
                                                    removed->size() == 1 ? "" : "s"));
    return {{"deleted", name}, {"revoked_tokens", revoked}};
}

json ApiService::setPassword(const std::string& name, const json& body, const Principal& by) {
    requireObject(body);
    // A token's name is free text, so only a signed-in user is "you"
    bool own = !by.token && by.name == name;
    if (by.role != Role::Admin && !own) throw ApiError(403, "forbidden", "you can only change your own password");
    if (!body.contains("password") || !body["password"].is_string()) throw invalid("give the new password");
    // Your own password: prove it is you (an unattended session must not lock the owner out). An
    // administrator resetting someone else's has no way to know theirs.
    std::optional<std::string> current;
    if (own) {
        if (!body.contains("current_password") || !body["current_password"].is_string()) {
            throw invalid("give your current password as \"current_password\" to change it");
        }
        current = body["current_password"].get<std::string>();
    }
    if (auto ok = auth_.setPassword(name, body["password"].get<std::string>(), by.session, current); !ok) {
        if (ok.error().starts_with("too many")) throw ApiError(429, "too_many_attempts", ok.error());
        throw invalid(ok.error());
    }
    accessEvent(&by, own ? std::format("user '{}' changed their password", name) : std::format("password of user '{}' reset", name));
    return {{"ok", true}};
}

json ApiService::tokens() const {
    json out = json::array();
    for (const auto& t : auth_.tokens()) {
        out.push_back({{"id", t.id}, {"name", t.name}, {"role", roleName(t.role)}, {"created_by", t.createdBy},
                       {"created", t.createdMs}, {"last_used", t.lastUsedMs}});
    }
    return out;
}

json ApiService::createToken(const json& body, const Principal& by) {
    requireObject(body);
    auto role = parseRole(body.contains("role") && body["role"].is_string() ? body["role"].get<std::string>() : body.contains("role") ? "" : "admin");
    if (!role) throw invalid("role must be admin or viewer");
    if (body.contains("name") && !body["name"].is_string()) throw invalid("name must be text");
    auto created = auth_.createToken(body.value("name", std::string()), *role, by.name, by.token ? std::string() : by.name);
    if (!created) throw invalid(created.error());
    const auto& [info, secret] = *created;
    accessEvent(&by, std::format("token '{}' created ({})", info.name, roleName(info.role)));
    return {{"id", info.id}, {"name", info.name}, {"role", roleName(info.role)}, {"token", secret},
            {"note", "copy the token now; it is not shown again"}};
}

void ApiService::revokeToken(const std::string& id, const Principal* by) {
    std::string name = id;
    for (const TokenInfo& t : auth_.tokens()) {
        if (t.id == id) name = t.name;
    }
    if (auto ok = auth_.revokeToken(id); !ok) throw notFound(ok.error());
    accessEvent(by, std::format("token '{}' revoked", name));
}

void ApiService::accessEvent(const Principal* by, std::string message) const {
    log::event(log::Level::Info, "web", engine_ ? engine_->config().plc.id : std::string(), "access", authorOf(by), std::move(message));
}

void ApiService::logProgramEvent(const std::string& id, const SaveOutcome& save, bool deploy, const Principal* by) const {
    const ProgramLoad& own = save.own;
    std::string what = save.library ? "library" : "program";
    std::string message;
    log::Level level = log::Level::Info;
    if (!deploy) {
        message = std::format("{} '{}' saved as a draft (not loaded)", what, save.program);
    } else if (!own.ok) {
        auto [line, error] = firstError(own.compiled);
        level = log::Level::Warn;
        message = error.empty() ? std::format("{} '{}' saved as a draft: it was not loaded ({})", what, save.program,
                                              !own.error.empty() ? own.error : own.taskErrors.empty() ? "refused" : own.taskErrors.begin()->second)
                                : std::format("{} '{}' saved as a draft: it does not compile (line {}: {})", what, save.program, line, error);
    } else if (own.tasks.empty()) {
        size_t took = static_cast<size_t>(std::count_if(save.dependents.begin(), save.dependents.end(), [](const ProgramLoad& d) { return d.ok; }));
        message = save.dependents.empty() ? std::format("{} '{}' saved", what, save.program)
                                          : std::format("{} '{}' saved: {} of {} running program{} took it over", what, save.program, took,
                                                        save.dependents.size(), save.dependents.size() == 1 ? "" : "s");
    } else if (!own.replaced) {
        message = std::format("program '{}' downloaded to task{} {}: every variable from its initial value", save.program,
                              own.tasks.size() == 1 ? "" : "s", listForEvent(own.tasks));
    } else {
        std::set<std::string> reset(own.reset.begin(), own.reset.end());
        size_t kept = own.compiled.image ? keptCount(*own.compiled.image, own.reset) : 0;
        message = std::format("program '{}' online change in task{} {}: {} value{} kept, {} reset{}", save.program,
                              own.tasks.size() == 1 ? "" : "s", listForEvent(own.tasks), kept, kept == 1 ? "" : "s", reset.size(),
                              reset.empty() ? "" : " (" + listForEvent(std::vector<std::string>(reset.begin(), reset.end())) + ")");
    }
    log::event(level, "plc", id, "program", authorOf(by), std::move(message));
}

// ───────────────────────────── Modbus client ─────────────────────────────

namespace {

/// A whole number field of a request, checked before it is converted.
int intField(const json& body, const char* key, int fallback, int min, int max) {
    if (!body.contains(key)) return fallback;
    const json& v = body[key];
    bool whole = v.is_number_integer() || (v.is_number_float() && std::trunc(v.get<double>()) == v.get<double>());
    double x = v.is_number() ? v.get<double>() : 0;
    if (!whole || x < min || x > max) throw invalid(std::format("{} must be a whole number from {} to {}", key, min, max));
    return static_cast<int>(x);
}

modbus::TcpClient clientFor(const json& body, Area& area, uint16_t& address) {
    requireObject(body);
    modbus::TcpClient::Options options;
    if (body.contains("host") && !body["host"].is_string()) throw invalid("host must be a host name or address");
    options.host = body.value("host", "127.0.0.1");
    int port = intField(body, "port", 502, 1, 65535);
    int unit = intField(body, "unit", 1, 0, 255);
    int timeout = intField(body, "timeout_ms", 1000, 10, 10000);
    int addr = intField(body, "address", 0, 0, 65535);
    if (body.contains("area") && !body["area"].is_string()) throw invalid("area must be coil, discrete_input, holding_register or input_register");
    area = areaArg(body.value("area", std::string()));
    address = static_cast<uint16_t>(addr);
    options.port = static_cast<uint16_t>(port);
    options.unit = static_cast<uint8_t>(unit);
    options.timeout = std::chrono::milliseconds(timeout);
    return modbus::TcpClient(options);
}

}  // namespace

json ApiService::modbusRead(const json& body) const {
    Area area;
    uint16_t address;
    modbus::TcpClient client = clientFor(body, area, address);
    int count = intField(body, "count", 1, 1, isBitArea(area) ? 2000 : 125);
    if (address + count > 65536) throw invalid(std::format("addresses {}-{} do not fit (0-65535)", address, address + count - 1));
    auto values = client.read(area, address, static_cast<uint16_t>(count));
    if (!values) throw ApiError(502, "device_error", values.error());
    json out = json::array();
    for (uint16_t v : *values) {
        if (isBitArea(area)) {
            out.push_back(v != 0);
        } else {
            out.push_back(v);
        }
    }
    return {{"area", areaName(area)}, {"address", address}, {"values", out}};
}

json ApiService::modbusWrite(const json& body) const {
    Area area;
    uint16_t address;
    modbus::TcpClient client = clientFor(body, area, address);
    json valuesJson = body.contains("values") ? body["values"] : json::array({body.value("value", json())});
    if (!valuesJson.is_array() || valuesJson.empty() || valuesJson[0].is_null()) {
        throw invalid("give \"values\": [...] or \"value\"");
    }
    // Refused here, before any traffic: these are the caller's mistakes (4xx), not the device's (502)
    if (!isClientWritable(area)) {
        throw invalid(std::format("{} entries are read-only over Modbus; write coil or holding_register", areaName(area)));
    }
    size_t most = isBitArea(area) ? modbus::kMaxWriteBits : modbus::kMaxWriteRegisters;
    if (valuesJson.size() > most) throw invalid(std::format("at most {} values per write", most));
    std::vector<uint16_t> raw;
    for (const json& v : valuesJson) {
        double x = jsonToValue(v, "values");
        if (isBitArea(area) && x != 0.0 && x != 1.0) throw invalid("coil values must be true/false or 0/1");
        if (!isBitArea(area) && (x != std::trunc(x) || x < -32768 || x > 65535)) {
            throw invalid("register values must be whole numbers from -32768 to 65535");
        }
        raw.push_back(isBitArea(area) ? (x != 0.0 ? 1 : 0) : static_cast<uint16_t>(static_cast<int32_t>(x)));
    }
    if (static_cast<size_t>(address) + raw.size() > 65536) {
        throw invalid(std::format("addresses {}-{} do not fit (0-65535)", address, static_cast<size_t>(address) + raw.size() - 1));
    }
    if (auto ok = client.write(area, address, raw); !ok) throw ApiError(502, "device_error", ok.error());
    return {{"ok", true}, {"area", areaName(area)}, {"address", address}, {"count", raw.size()}};
}

// ───────────────────────────── events, probing and pairing ─────────────────────────────

json ApiService::setVariables(const std::string& id, const json& body) { return setVariables(id, body, nullptr); }

void ApiService::deleteProgram(const std::string& id, const std::string& name, const Principal* by) {
    deleteProgram(id, name);
    log::event(log::Level::Info, "plc", id, "program", authorOf(by), std::format("program '{}' deleted (its history keeps it)", name));
}

json ApiService::events(const std::string& id, size_t limit) const {
    plc(id);  // 404 for another id; a device's events are its PLC's
    json out = json::array();
    for (const log::Record& r : log::events(std::clamp<size_t>(limit, 1, 500))) {
        json by = r.by ? json{{"kind", r.by->kind}, {"name", r.by->name}} : json(nullptr);
        out.push_back({{"seq", r.seq}, {"t_ms", r.timeMs}, {"event", r.event}, {"message", r.message}, {"by", by},
                       {"level", log::levelName(r.level)}});
    }
    return {{"events", out}};
}

json ApiService::probe(const std::string& id, const json& body) const {
    requireObject(body);
    plc(id);
    if (!body.contains("host") || !body["host"].is_string() || body["host"].get<std::string>().empty()) {
        throw invalid("give the device's \"host\": a name or an address");
    }
    modbus::TcpClient::Options options;
    options.host = body["host"].get<std::string>();
    options.port = static_cast<uint16_t>(intField(body, "port", 502, 1, 65535));
    options.unit = static_cast<uint8_t>(intField(body, "unit", 1, 0, 255));
    int timeoutMs = intField(body, "timeout_ms", 500, 10, 10000);
    options.timeout = std::chrono::milliseconds(timeoutMs);
    // A client of its own, never the PLC's links, and one deadline for the whole probe: a device that does not
    // answer costs the caller seconds, not minutes
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::milliseconds(std::clamp(timeoutMs * 6, 1000, 20000));
    auto fresh = [&] {
        auto c = std::make_unique<modbus::TcpClient>(options);
        c->setInterrupt([deadline] { return Clock::now() < deadline; });
        return c;
    };
    auto client = fresh();
    std::optional<double> latencyMs;
    auto answered = [&](Clock::time_point since) {
        if (!latencyMs) latencyMs = round3(std::chrono::duration<double, std::milli>(Clock::now() - since).count());
    };

    json out = {{"ok", false}, {"latency_ms", nullptr}, {"identity", nullptr}, {"areas", json::object()}};
    auto started = Clock::now();
    auto identity = client->readIdentity();
    if (identity) {
        answered(started);
        out["identity"] = {{"vendor", identity->vendor}, {"product", identity->product}, {"revision", identity->revision}};
    } else if (client->lastFailure() == modbus::TcpClient::Failure::Device) {
        answered(started);  // an exception (not supported) is an answer too
    }
    std::string unreachable = !latencyMs ? identity.error() : std::string();
    for (Area area : {Area::Coil, Area::DiscreteInput, Area::HoldingRegister, Area::InputRegister}) {
        json a = {{"start", 0}, {"values", nullptr}};
        // Each area on its own: after no answer, a new connection, so one area's timeout costs the next nothing
        auto failure = client->lastFailure();
        if (failure == modbus::TcpClient::Failure::Transport || failure == modbus::TcpClient::Failure::Cancelled) client = fresh();
        uint16_t count = 64;
        while (true) {
            if (Clock::now() >= deadline) {
                a["error"] = "no time left: the probe's deadline passed";
                break;
            }
            auto sent = Clock::now();
            auto values = client->read(area, 0, count);
            if (values) {
                answered(sent);
                json list = json::array();
                for (uint16_t v : *values) {
                    if (isBitArea(area)) list.push_back(v != 0);
                    else list.push_back(v);
                }
                a["values"] = list;
                break;
            }
            bool device = client->lastFailure() == modbus::TcpClient::Failure::Device;
            if (device) answered(sent);
            // Exception 2 (illegal data address): fewer entries exist; halve down to one
            if (device && values.error().starts_with("device returned exception 2 ") && count > 1) {
                count = static_cast<uint16_t>(count / 2);
                continue;
            }
            a["error"] = values.error();
            break;
        }
        out["areas"][std::string(areaName(area))] = a;
    }
    out["ok"] = latencyMs.has_value();
    out["latency_ms"] = latencyMs ? json(*latencyMs) : json(nullptr);
    if (!latencyMs) out["error"] = unreachable.empty() ? std::string("the device did not answer") : unreachable;
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    log::write(log::Level::Debug, "modbus", id, std::format("probe of {}:{} unit {}: {} in {} ms", options.host, options.port, options.unit,
                                                          latencyMs ? "answered" : "no answer", took));
    return out;
}

json ApiService::hello() const {
    json plcId = nullptr;
    if (engine_) plcId = engine_->config().plc.id;
    return {{"product", "SimplePLC"}, {"version", kVersion}, {"plc", plcId}, {"needs_setup", auth_.needsSetup()},
            {"mode", engine_ ? "device" : "fleet"}};
}

json ApiService::pairingCode(const Principal* by) {
    requireEngine();  // a fleet is not adopted by another fleet
    Auth::PairingCode code = auth_.newPairingCode();
    accessEvent(by, "pairing code created: a fleet can adopt this device with it for 10 minutes");
    return {{"code", code.code}, {"expires_ms", code.expiresMs}};
}

json ApiService::pair(const json& body, const std::string& clientAddress) {
    Engine& engine = requireEngine();
    requireObject(body);
    if (!body.contains("code") || !body["code"].is_string() || !body.contains("fleet") || !body["fleet"].is_string()) {
        throw ApiError(400, "bad_request", "send {\"code\": \"<the pairing code from the device>\", \"fleet\": \"<the fleet's name>\"}");
    }
    std::string fleet = body["fleet"].get<std::string>();
    auto paired = auth_.pair(body["code"].get<std::string>(), fleet, clientAddress);
    if (!paired) {
        auto [status, message] = paired.error();
        throw ApiError(status, status == 403 ? "wrong_pairing_code" : status == 429 ? "too_many_attempts" : "invalid", message);
    }
    const auto& [info, secret] = *paired;
    Principal fleetToken{info.name, Role::Admin, true, {}, {}};
    accessEvent(&fleetToken, std::format("adopted by fleet '{}' from {}: token '{}' created (admin)", fleet, clientAddress, info.name));
    return {{"token", secret}, {"plc", engine.config().plc.id}};
}

json ApiService::unpair(const std::string& bearer, const Principal* by) {
    auto revoked = auth_.revokeBearer(bearer);
    if (!revoked) throw ApiError(409, "conflict", std::format("{}; only a token from pairing or POST /api/v1/tokens can leave", revoked.error()));
    accessEvent(by, std::format("token '{}' revoked: its fleet removed this device", revoked->name));
    return {{"revoked", revoked->id}, {"name", revoked->name}};
}

}  // namespace simpleplc::web
