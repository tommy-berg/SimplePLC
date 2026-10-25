#include "web/mcp.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "app/examples.h"
#include "app/version.h"

namespace simpleplc::web {
namespace {

constexpr const char* kProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

constexpr const char* kInstructions = R"(SimplePLC is a soft PLC. Each SimplePLC device (a process, a container or a
Raspberry Pi) runs one PLC: a process image (coils, discrete inputs, holding registers, input registers),
named tags on top of it, and IEC 61131-3 Structured Text programs that run every cycle. Every PLC serves
real Modbus TCP (as a server) and OPC UA; its `links` make it a Modbus TCP client of other devices.
A fleet (simpleplc fleet) gives one endpoint for the PLCs of many devices; the `plc` argument picks one.

Typical workflow:
1. list_plcs to see the PLCs (one on a device; all devices' PLCs in a fleet) and their Modbus/OPC UA ports.
2. get_language_reference: the Structured Text SimplePLC accepts (strictly typed; it refuses what it
   cannot run exactly as written, and every diagnostic says how to fix it).
3. setup_plc to set a PLC up from scratch (name, tags, program; or an example, see list_examples),
   or set_tags and set_program to change it step by step. A program reads and writes tags it declares
   in VAR_EXTERNAL (same name, matching type).
4. check_program to compile a program before deploying it (it also says which values an online change
   would keep); set_program reports diagnostics with line and column, and the old program keeps running
   until the new one compiles (deploy=false keeps a draft). A deployed change keeps the values of
   variables that still exist. SimplePLC has no simulator: test on a PLC that is not driving equipment,
   or against external tools (a Modbus client, Factory I/O, an OPC UA client), with read_values,
   get_variables, write_values and force_values.
5. read_values / write_values for tags, get_variables / set_variables for program variables, get_plc for
   state, faults (code and line), cycle times and latency; get_device for the hardware it runs on.
A runtime error (overflow, division by zero, index out of range, ...) puts the PLC in FAULT: programs stop,
tags with a `substitute` value get it, communication keeps running; get_plc shows where; run_plc restarts warm
(RETAIN variables keep their values, all others start from their initial values).
To connect PLCs, the one that polls (the Modbus TCP client) lists the other device in its configuration's `links`
(host, port, which of its tags to read from and write to which addresses); modbus_read/modbus_write check any device from outside, and
probe_device shows what a device holds before you link it. get_events says who changed what on a PLC.)";

struct RpcError {
    int code;
    std::string message;
};

/// JSON-RPC 2.0 and MCP error codes.
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kResourceNotFound = -32002;

/// A tool's arguments: every property is described, and no other is accepted (a misspelt one is
/// refused, never ignored).
json schema(json properties, std::vector<std::string> required = {}) {
    json j = {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) j["required"] = required;
    return j;
}

json str(const char* description) { return {{"type", "string"}, {"description", description}}; }
json integer(const char* description, int64_t min, int64_t max) {
    return {{"type", "integer"}, {"description", description}, {"minimum", min}, {"maximum", max}};
}
json boolean(const char* description) { return {{"type", "boolean"}, {"description", description}}; }
json object(const char* description) { return {{"type", "object"}, {"description", description}}; }
json array(const char* description, json items) {
    return {{"type", "array"}, {"description", description}, {"items", std::move(items)}};
}

const json kTagSchema = {
    {"type", "object"},
    {"properties",
     {{"name", str("Tag name: letters, digits and single underscores, starting with a letter or _, not ending with _; "
                   "not a keyword or standard name (IF, TON, ...). Case does not matter: Level and level are one name")},
      {"area", {{"type", "string"}, {"enum", {"coil", "discrete_input", "holding_register", "input_register"}}}},
      {"address", integer("First address (0-based); each address belongs to one tag", 0, 65535)},
      {"type", {{"type", "string"},
                {"enum", {"bool", "uint16", "int16", "uint32", "int32", "float32"}},
                {"description", "bool for coil/discrete_input; a number type for registers (default uint16). 32-bit types use two registers."}}},
      {"description", str("What the tag means")},
      {"substitute", {{"type", {"boolean", "number", "null"}},
                {"description", "Substitute value: written to the tag when the PLC stops or faults, e.g. false for a motor or "
                                "0.0 for a drive; leave it out for tags that keep their value"}}},
      {"retain", boolean("Retentive: the value is kept across a power cycle (written to retain.json when it changes, "
                         "at most once a second, and restored at power-up; a cold restart clears it). For setpoints and "
                         "totals, not for inputs")}}},
    {"required", {"name", "area", "address"}},
    {"additionalProperties", false}};

const json kTaskSchema = {
    {"type", "object"},
    {"properties",
     {{"name", str("Task name, e.g. main")},
      {"program", str("Program it runs (a program file name)")},
      {"cycle_ms", integer("Cycle time in ms (default 10)", 1, 60000)},
      {"enabled", boolean("Runs in RUN mode (default true)")},
      {"priority", integer("Real-time priority 1-98 with \"realtime\"; 0: normal scheduling", 0, 98)},
      {"watchdog_ms", integer("Wall-clock limit per cycle in ms; 0: max(100, 10 x cycle_ms)", 0, 600000)}}},
    {"required", {"name", "program"}},
    {"additionalProperties", false}};

const json kConfigVersionArgument = {
    {"type", "integer"},
    {"minimum", 0},
    {"description", "Apply only if the configuration still has this config_version (from get_plc or list_plcs); "
                    "otherwise nothing changes and the error says so. Use it when others may change the PLC too"}};

// ───────────────────────────── arguments, checked against the schemas ─────────────────────────────

size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

/// The known name closest to `key`, if it is close enough to be a misspelling.
std::string closest(const std::string& key, const json& properties) {
    std::string best;
    size_t bestDistance = std::string::npos;
    for (const auto& [name, unused] : properties.items()) {
        size_t d = editDistance(key, name);
        if (name.starts_with(key) || key.starts_with(name)) d = std::min<size_t>(d, 1);
        if (d < bestDistance) bestDistance = d, best = name;
    }
    return bestDistance <= std::max<size_t>(2, key.size() / 3) ? best : std::string();
}

/// A JSON library error without the library's numbering ("[json.exception.type_error.302] ").
std::string plainJsonError(const json::exception& e) {
    std::string what = e.what();
    if (what.starts_with("[json.exception.")) what.erase(0, what.find("] ") + 2);
    return what;
}

/// What a value is, for messages: "the string \"5\"", "a number (2.5)", "an object".
std::string describe(const json& v) {
    std::string text = dumpJson(v);
    if (text.size() > 40) text = text.substr(0, 37) + "...";
    if (v.is_string()) return "the string " + text;
    if (v.is_boolean()) return text;
    if (v.is_number()) return "the number " + text;
    if (v.is_null()) return "null";
    return v.is_object() ? "an object" : "an array";
}

std::string typeName(const std::string& type) {
    if (type == "integer") return "a whole number";
    if (type == "number") return "a number";
    if (type == "boolean") return "true or false";
    if (type == "object") return "an object";
    if (type == "array") return "an array";
    if (type == "null") return "null";
    return "a string";
}

bool hasType(json& v, const std::string& type) {
    if (type == "string") return v.is_string();
    if (type == "number") return v.is_number();
    if (type == "boolean") return v.is_boolean();
    if (type == "object") return v.is_object();
    if (type == "array") return v.is_array();
    if (type == "null") return v.is_null();
    if (type == "integer") {
        if (v.is_number_integer()) return true;
        // 5.0 is a whole number too (JSON Schema): pass it on as one
        if (v.is_number_float() && std::trunc(v.get<double>()) == v.get<double>() && std::abs(v.get<double>()) <= 9007199254740992.0) {
            v = static_cast<int64_t>(v.get<double>());
            return true;
        }
    }
    return false;
}

/// Checks `value` against `schema` (type, enum, minimum/maximum, properties, required,
/// additionalProperties, items); a whole number given as 5.0 becomes 5. `path` names the value.
void checkValue(const json& schema, json& value, const std::string& path, std::vector<std::string>& problems) {
    if (schema.contains("type")) {
        std::vector<std::string> types;
        if (schema["type"].is_string()) types.push_back(schema["type"].get<std::string>());
        else for (const json& t : schema["type"]) types.push_back(t.get<std::string>());
        bool matched = false;
        for (const std::string& t : types) matched = matched || hasType(value, t);
        if (!matched) {
            std::string expected;
            for (size_t i = 0; i < types.size(); i++) expected += (i == 0 ? "" : i + 1 == types.size() ? " or " : ", ") + typeName(types[i]);
            problems.push_back(std::format("{} must be {}, not {}", path, expected, describe(value)));
            return;
        }
    }
    if (schema.contains("enum")) {
        bool known = false;
        std::string list;
        for (const json& option : schema["enum"]) {
            known = known || option == value;
            list += (list.empty() ? "" : ", ") + dumpJson(option);
        }
        if (!known) problems.push_back(std::format("{} must be one of {}, not {}", path, list, dumpJson(value)));
    }
    if (value.is_number() && (schema.contains("minimum") || schema.contains("maximum"))) {
        double x = value.get<double>();
        bool low = schema.contains("minimum") && x < schema["minimum"].get<double>();
        bool high = schema.contains("maximum") && x > schema["maximum"].get<double>();
        if (low || high) {
            problems.push_back(schema.contains("maximum") && schema.contains("minimum")
                                   ? std::format("{} must be from {} to {}, not {}", path, dumpJson(schema["minimum"]), dumpJson(schema["maximum"]), dumpJson(value))
                                   : schema.contains("minimum") ? std::format("{} must be {} or more, not {}", path, dumpJson(schema["minimum"]), dumpJson(value))
                                                                : std::format("{} must be {} or less, not {}", path, dumpJson(schema["maximum"]), dumpJson(value)));
        }
    }
    if (value.is_object() && schema.contains("properties")) {
        const json& properties = schema["properties"];
        bool top = path.empty();
        auto name = [&](const std::string& key) { return top ? std::format("'{}'", key) : std::format("{}.{}", path, key); };
        for (const json& key : schema.value("required", json::array())) {
            if (!value.contains(key.get<std::string>()) || value[key.get<std::string>()].is_null()) {
                std::string what = properties.contains(key) ? properties[key.get<std::string>()].value("description", "") : "";
                problems.push_back(std::format("{} is required{}", name(key.get<std::string>()), what.empty() ? "" : " (" + what + ")"));
            }
        }
        for (auto& [key, v] : value.items()) {
            if (properties.contains(key)) {
                if (!v.is_null() || !properties[key].contains("type")) checkValue(properties[key], v, name(key), problems);
                continue;
            }
            if (schema.value("additionalProperties", true) == false) {
                std::string guess = closest(key, properties);
                std::string known;
                for (const auto& [k, unused] : properties.items()) known += (known.empty() ? "" : ", ") + k;
                problems.push_back(guess.empty() ? std::format("{} is not {} (known: {})", name(key), top ? "an argument of this tool" : "a known key", known)
                                                 : std::format("{} is not {} (did you mean '{}'?)", name(key), top ? "an argument of this tool" : "a known key", guess));
            }
        }
    }
    if (value.is_array() && schema.contains("items")) {
        for (size_t i = 0; i < value.size(); i++) {
            checkValue(schema["items"], value[i], std::format("{}[{}]", path.empty() ? "arguments" : path, i), problems);
        }
    }
}

/// MCP 2025-06-18 tool annotations: what a host may run without asking. Destructive: it stops,
/// overwrites, forces or writes outputs or configuration. Idempotent: running it again with the same
/// arguments changes nothing more.
struct Hints {
    bool destructive;
    bool idempotent;
};
Hints hintsFor(const std::string& tool, bool readOnly) {
    if (readOnly) return {false, true};
    static const std::map<std::string, Hints, std::less<>> hints = {
        {"setup_plc", {true, false}},       {"update_plc", {true, true}},      {"run_plc", {true, true}},
        {"stop_plc", {true, true}},         {"restart_plc", {true, false}},    {"set_tags", {true, true}},
        {"write_values", {true, true}},     {"write_area", {true, true}},      {"force_values", {true, true}},
        {"release_forces", {true, true}},   {"set_program", {true, true}},     {"update_task", {true, true}},
        {"set_variables", {true, true}},    {"modbus_write", {true, true}},
        {"delete_program", {true, true}},   {"restore_program", {true, true}},
        {"add_device", {false, false}},     {"remove_device", {true, true}},
    };
    auto it = hints.find(tool);
    return it != hints.end() ? it->second : Hints{true, false};  // anything new: assume the worst
}

/// ChangeOptions of a tool that changes the configuration: config_version and force.
ChangeOptions changeOptions(const json& args) {
    ChangeOptions options;
    if (args.contains("config_version")) options.ifVersion = args["config_version"].get<uint64_t>();
    options.force = args.value("force", false);
    return options;
}

constexpr const char* kStMime = "text/x-structured-text";

const json kAreaSchema = {{"type", "string"},
                          {"enum", {"coil", "discrete_input", "holding_register", "input_register"}}};

std::string requireString(const json& args, const char* key) {
    auto it = args.find(key);
    if (it == args.end() || !it->is_string() || it->get<std::string>().empty()) {
        throw ApiError(422, "invalid", std::format("'{}' is required", key));
    }
    return it->get<std::string>();
}

std::string optionalString(const json& args, const char* key, std::string fallback = {}) {
    auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : fallback;
}

json toolResult(const json& data) {
    // A tool whose text is not its JSON (get_program: the source as text, its version as data) puts the text in "_text"
    if (data.is_object() && data.contains("_text") && data["_text"].is_string()) {
        json structured = data;
        structured.erase("_text");
        return {{"content", json::array({{{"type", "text"}, {"text", data["_text"].get<std::string>()}}})},
                {"isError", false},
                {"structuredContent", structured}};
    }
    json result = {{"content", json::array({{{"type", "text"}, {"text", data.is_string() ? data.get<std::string>() : dumpJson(data, 2)}}})},
                   {"isError", false}};
    // structuredContent is an object, and the text is its serialisation (MCP 2025-06-18): a list has none
    if (data.is_object()) result["structuredContent"] = data;
    return result;
}

/// A diagnostic as one line: "main.st line 3, column 5: error E_TYPE: ... (hint: ...)".
std::string diagnosticLine(const json& d) {
    std::string line = std::format("{} line {}, column {}: {} {}: {}", d.value("file", ""), d.value("line", 0), d.value("column", 0),
                                   d.value("severity", ""), d.value("code", ""), d.value("message", ""));
    if (!d.value("hint", "").empty()) line += std::format(" (hint: {})", d.value("hint", ""));
    return line;
}

json toolError(const std::string& message, const json& details = nullptr) {
    std::string text = message;
    if (details.is_object() && details.contains("problems") && details["problems"].is_array()) {
        const json& problems = details["problems"];
        // One problem is the message itself; several are listed, each once
        if (problems.size() > 1) {
            text = std::format("{} problems:", problems.size());
            for (const auto& p : problems) text += "\n- " + (p.is_string() ? p.get<std::string>() : dumpJson(p));
        } else if (problems.size() == 1 && problems[0].is_string() && problems[0].get<std::string>() != message) {
            text += "\n- " + problems[0].get<std::string>();
        }
    }
    // Compiler diagnostics: every one, with its position and hint, as set_program gives them
    if (details.is_object() && details.contains("diagnostics") && details["diagnostics"].is_array()) {
        for (const json& d : details["diagnostics"]) {
            if (d.is_object()) text += "\n" + diagnosticLine(d);
        }
    }
    json result = {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", true}};
    return result;
}

/// The `plc` argument; it may be left out where there is only one PLC (a device).
std::string plcArgument(ApiService& api, const json& args) {
    std::string id = optionalString(args, "plc");
    if (!id.empty()) return id;
    json plcs = api.listPlcs();
    if (plcs.size() == 1) return plcs[0]["id"].get<std::string>();
    throw ApiError(422, "invalid", "'plc' is required (see list_plcs)");
}

const json kPlcArgument = str("PLC id (may be left out on a device, which has one PLC)");

std::string primaryProgram(ApiService& api, const std::string& plc, const std::string& task) {
    json config = api.getPlc(plc)["config"];
    for (const auto& t : config["tasks"]) {
        if (task.empty() || t["name"] == task) return t["program"].get<std::string>();
    }
    throw ApiError(404, "not_found", std::format("no task named '{}'", task));
}

/// Tools that connect to other hosts (a viewer must not use the server to probe the network).
bool reachesOtherHosts(const McpServer::Tool& tool) {
    return tool.name.starts_with("modbus_") || tool.name == "probe_device" || tool.name == "add_device";
}

/// A program that did not load: every diagnostic as file:line:column, code, message and hint, then
/// the tasks that refused it (e.g. a tag with two writers).
std::string loadFailure(const std::string& what, const json& result) {
    std::string text = what;
    for (const json& d : result.value("diagnostics", json::array())) {
        if (!d.is_object()) continue;
        text += std::format("\n{} line {}, column {}: {} {}: {}", d.value("file", ""), d.value("line", 0), d.value("column", 0),
                            d.value("severity", ""), d.value("code", ""), d.value("message", ""));
        if (!d.value("hint", "").empty()) text += std::format(" (hint: {})", d.value("hint", ""));
    }
    for (const json& r : result.value("results", json::array())) {
        if (r.is_object() && !r.value("ok", true) && r.contains("error") && r["error"].is_string()) {
            text += std::format("\ntask '{}': {}", r.value("task", ""), r["error"].get<std::string>());
        }
    }
    // The same as data, for a client that reads fields
    json details = {{"diagnostics", result.value("diagnostics", json::array())}, {"results", result.value("results", json::array())}};
    return text + "\n\n" + dumpJson(details, 2);
}

}  // namespace

McpServer::McpServer(ApiService& api, Fleet* fleet) : api_(api), fleet_(fleet) {
    auto add = [this](Tool tool) { tools_.push_back(std::move(tool)); };

    add({"list_plcs", "List PLCs",
         "All PLCs with their state, Modbus/OPC UA ports, task statistics, forced values, warnings and config_version.",
         schema(json::object()), true, false,
         [this](const json&, const Principal&) { return api_.listPlcs(); }});

    add({"get_plc", "Get a PLC",
         "One PLC in detail: configuration (tags, tasks, ports) and its config_version, state, cycle times, errors, "
         "forced values and connected clients.",
         schema({{"plc", kPlcArgument}}), true, false,
         [this](const json& a, const Principal&) { return api_.getPlc(plcArgument(api_, a)); }});

    add({"get_device", "Get the device",
         "The device a PLC runs on, like a device manager: model (e.g. a Raspberry Pi), container, OS, CPU load and "
         "temperature, memory, storage, network interfaces and addresses, and SimplePLC's own memory use.",
         schema({{"plc", kPlcArgument}}), true, false,
         [this](const json& a, const Principal&) { return api_.device(plcArgument(api_, a)); }});

    add({"setup_plc", "Set up a PLC",
         "Sets a PLC up from scratch in one call and cold-starts it: name, tags, program and cycle time, and any other "
         "setting of the configuration (see get_plc → config). Pass `example` (see list_examples) for tags and programs "
         "that already work, or \"none\" for a blank PLC. The id and the network settings (modbus, opcua, device) stay "
         "unless given, so other devices keep reaching it. A program that does not compile changes nothing.",
         schema({{"plc", kPlcArgument},
                 {"name", str("Display name")},
                 {"description", str("What this PLC does")},
                 {"example", str("Start from a bundled example, e.g. tank, blink, traffic-light, remote-io; \"none\" for a blank PLC")},
                 {"program", str("Structured Text source of the main task's program (PROGRAM name ... END_PROGRAM)")},
                 {"tags", array("Tag table", kTagSchema)},
                 {"cycle_ms", integer("Cycle time of the main (first) task in ms, also of an example's (default 10); "
                                     "with `tasks`, give each task its cycle_ms instead", 1, 60000)},
                 {"tasks", array("Tasks instead of one main task", kTaskSchema)},
                 {"programs", object("Program name -> Structured Text source, for several tasks")},
                 {"links", array("Modbus links to other devices (see update_plc and get_language_reference)", {{"type", "object"}})},
                 {"image_size", integer("Entries per area (default 1024)", 1, 65536)},
                 {"autostart", boolean("Start the programs when SimplePLC starts (default true)")},
                 {"realtime", boolean("Real-time scheduling for tasks with a priority (default false)")},
                 {"modbus", object("Modbus TCP server: {enabled, listen, port, max_connections, idle_timeout_s}; "
                                   "what is left out stays as it is")},
                 {"opcua", object("OPC UA server: {enabled, listen, port}; what is left out stays as it is")},
                 {"device", object("Modbus device identity: {unit_id, vendor, product_code, model}")},
                 {"run", boolean("Start the program right away (default true)")},
                 {"config_version", kConfigVersionArgument}}),
         false, true,
         [this](const json& a, const Principal& p) {
             json body = a;
             for (const char* key : {"plc", "config_version"}) body.erase(key);
             json result = api_.setupPlc(plcArgument(api_, a), body, changeOptions(a), &p);
             if (result["loaded"] == false) {
                 throw ApiError(422, "load_failed", loadFailure("the PLC is set up, but a program failed to load:", result));
             }
             return result;
         }});

    add({"update_plc", "Change a PLC's settings",
         "Applies a JSON merge patch to the PLC configuration, e.g. {\"name\": \"Pump station\"}, "
         "{\"modbus\": {\"port\": 5021}} or {\"tasks\": [...]}. Arrays are replaced as a whole. Only what changed restarts: "
         "tasks that stay keep their program and values, and the answer's restarted_tasks lists those that started from "
         "initial values. Changing \"id\" or image_size restarts the PLC. A task naming a program that does not exist, or one "
         "that does not compile, is refused. Unknown keys are refused. Give config_version (from get_plc) so a change "
         "someone else made meanwhile is not overwritten.",
         schema({{"plc", kPlcArgument},
                 {"changes", object("Merge patch for the configuration (see get_plc → config); \"create\": true in it lets "
                                    "tasks name programs that do not exist yet (they start from an empty template)")},
                 {"config_version", kConfigVersionArgument},
                 {"force", boolean("Apply a tag change although a running program no longer compiles with it (the PLC then faults); "
                                   "without it such a change is refused and the programs are listed")}},
                {"changes"}),
         false, false,
         [this](const json& a, const Principal& p) {
             return api_.updatePlc(plcArgument(api_, a), a["changes"], changeOptions(a), &p);
         }});

    add({"run_plc", "Run (RUN mode)",
         "Starts the PLC's programs (RUN). After a FAULT it restarts them warm: RETAIN variables keep their values, all "
         "others start from their initial values. Communication runs in both RUN and STOP. The process powers up in "
         "the mode commanded last.",
         schema({{"plc", kPlcArgument}}), false, false,
         [this](const json& a, const Principal& p) { return api_.runPlc(plcArgument(api_, a), &p); }});

    add({"stop_plc", "Stop (STOP mode)",
         "Stops the PLC's programs (STOP). Tags with a `substitute` value get it; other tags keep "
         "their values and stay readable and writable, and communication keeps running. Forced values stay forced in "
         "STOP: release them (release_forces) for their substitute values. A STOP survives a restart of the process: the PLC "
         "powers up in STOP until run_plc.",
         schema({{"plc", kPlcArgument}}), false, false,
         [this](const json& a, const Principal& p) { return api_.stopPlc(plcArgument(api_, a), &p); }});

    add({"restart_plc", "Cold restart",
         "Cold restart: closes and opens the PLC again (communication too): tags start from 0, program variables "
         "(RETAIN too) from their initial values, retentive memory is cleared, the programs load again from their files, then RUN.",
         schema({{"plc", kPlcArgument}}), false, false,
         [this](const json& a, const Principal& p) { return api_.restartPlc(plcArgument(api_, a), &p); }});

    add({"set_tags", "Set the tag table",
         "Replaces all tags of a PLC with the given list (tags left out are deleted). Tags name addresses so programs, "
         "OPC UA and these tools can use names. Names are unique without regard to case and usable in Structured Text; "
         "each address belongs to one tag. A change that would make a running program fail to compile (a tag it uses "
         "removed, renamed or retyped) is refused with the programs and their diagnostics: change the program first, or "
         "pass force (the PLC then faults). Give config_version (from get_plc) so a change someone else made meanwhile "
         "is not overwritten.",
         schema({{"plc", kPlcArgument},
                 {"tags", array("Complete tag table", kTagSchema)},
                 {"config_version", kConfigVersionArgument},
                 {"force", boolean("Apply it although a running program no longer compiles (the PLC faults)")}},
                {"tags"}),
         false, false,
         [this](const json& a, const Principal& p) { return api_.setTags(plcArgument(api_, a), a["tags"], changeOptions(a), &p); }});

    add({"read_values", "Read tag values", "Current values of the given tags, or of all tags.",
         schema({{"plc", kPlcArgument}, {"tags", array("Tag names (all tags if omitted)", {{"type", "string"}})}}),
         true, false,
         [this](const json& a, const Principal&) {
             std::vector<std::string> names;
             for (const auto& n : a.value("tags", json::array())) names.push_back(n.get<std::string>());
             return api_.values(plcArgument(api_, a), names);
         }});

    add({"write_values", "Write tag values",
         "Writes tags atomically, e.g. {\"pump\": true, \"setpoint\": 42.5}. The program may overwrite them on its next cycle.",
         schema({{"plc", kPlcArgument}, {"values", object("Tag name -> value (true/false or number)")}}, {"values"}),
         false, false,
         [this](const json& a, const Principal& p) { return api_.writeValues(plcArgument(api_, a), a["values"], &p); }});

    add({"read_area", "Read raw addresses", "Reads entries of one area by address, without tags.",
         schema({{"plc", kPlcArgument}, {"area", kAreaSchema}, {"start", integer("First address", 0, 65535)},
                 {"count", integer("Number of entries (default 1)", 1, 2000)}},
                {"area", "start"}),
         true, false,
         [this](const json& a, const Principal&) {
             return api_.readArea(plcArgument(api_, a), requireString(a, "area"), a["start"].get<int>(), a.value("count", 1));
         }});

    add({"write_area", "Write raw addresses", "Writes entries of one area by address (bits as true/false, registers -32768..65535).",
         schema({{"plc", kPlcArgument}, {"area", kAreaSchema}, {"start", integer("First address", 0, 65535)},
                 {"values", array("Values", {{"type", {"boolean", "number"}}})}},
                {"area", "start", "values"}),
         false, false,
         [this](const json& a, const Principal& p) {
             return api_.writeArea(plcArgument(api_, a), requireString(a, "area"), {{"start", a["start"]}, {"values", a["values"]}}, &p);
         }});

    add({"force_values", "Force tags",
         "Forces tags to values: they keep these values and ignore every write (program, Modbus, OPC UA) until released, "
         "and keep them in STOP and FAULT too (instead of their substitute values). get_plc shows how many are forced.",
         schema({{"plc", kPlcArgument}, {"values", object("Tag name -> forced value")}}, {"values"}),
         false, false,
         [this](const json& a, const Principal& p) { return api_.force(plcArgument(api_, a), a["values"], &p); }});

    add({"release_forces", "Release forced tags",
         "Releases the given forced tags, or all of them.",
         schema({{"plc", kPlcArgument}, {"tags", array("Tag names (all if omitted)", {{"type", "string"}})}}),
         false, false,
         [this](const json& a, const Principal& p) {
             json body = a.contains("tags") ? json{{"tags", a["tags"]}} : json::object();
             return api_.release(plcArgument(api_, a), body, &p);
         }});

    add({"list_forces", "List forced values", "The forced entries of the PLC: area, address, value and the tag they belong to.",
         schema({{"plc", kPlcArgument}}), true, false,
         [this](const json& a, const Principal&) { return api_.forces(plcArgument(api_, a)); }});

    add({"list_programs", "List programs",
         "The PLC's program files with size, last change and the tasks that run each (used_by); files no task runs "
         "are libraries of the others.",
         schema({{"plc", kPlcArgument}}), true, false,
         [this](const json& a, const Principal&) { return api_.programs(plcArgument(api_, a)); }});

    add({"delete_program", "Delete a program", "Deletes a program file that no task runs.",
         schema({{"plc", kPlcArgument}, {"program", str("Program name")}}, {"program"}), false, false,
         [this](const json& a, const Principal& p) {
             std::string program = requireString(a, "program");
             api_.deleteProgram(plcArgument(api_, a), program, &p);
             return json{{"deleted", program}};
         }});

    add({"get_program_history", "Earlier versions of a program",
         "The earlier versions of a program (newest first), or with `version` that version's Structured Text source.",
         schema({{"plc", kPlcArgument}, {"program", str("Program name")}, {"version", str("A version from the list")}},
                {"program"}),
         true, false,
         [this](const json& a, const Principal&) {
             std::string plc = plcArgument(api_, a);
             if (a.contains("version")) return json(api_.programVersion(plc, requireString(a, "program"), requireString(a, "version")));
             return api_.programHistory(plc, requireString(a, "program"));
         }});

    add({"restore_program", "Restore an earlier version",
         "Saves an earlier version of a program as the current one and loads it, like set_program (the version it "
         "replaces goes to the history).",
         schema({{"plc", kPlcArgument}, {"program", str("Program name")}, {"version", str("A version from get_program_history")},
                 {"deploy", boolean("Load it into the tasks now (default true)")}},
                {"program", "version"}),
         false, false,
         [this](const json& a, const Principal& p) {
             json result = api_.restoreProgram(plcArgument(api_, a), requireString(a, "program"), requireString(a, "version"),
                                               a.value("deploy", true), &p);
             if (result["loaded"] == false) {
                 throw ApiError(422, "load_failed", loadFailure("the version was restored, but it failed to load; the previous version keeps running:", result));
             }
             return result;
         }});

    add({"get_program", "Get a program",
         "Structured Text source of a program file (by default the main task's program, the first task's). The text is the "
         "source; the structured result adds its hash: pass it to set_program as base_hash so a save fails when someone "
         "else saved the program meanwhile. deployed: the version the tasks run (what a restart runs), with the library "
         "files it was compiled with; draft says whether the saved file differs from it.",
         schema({{"plc", kPlcArgument}, {"program", str("Program name")}, {"task", str("Task whose program to get")},
                 {"deployed", boolean("The version the tasks run instead of the saved file (default false)")}}),
         true, false,
         [this](const json& a, const Principal&) {
             std::string plc = plcArgument(api_, a);
             std::string program = optionalString(a, "program");
             if (program.empty()) program = primaryProgram(api_, plc, optionalString(a, "task"));
             if (a.value("deployed", false)) {
                 json d = api_.deployedProgram(plc, program);
                 const json& own = d["files"][0];
                 return json{{"_text", own["source"]}, {"source", own["source"]}, {"hash", own["hash"]}, {"deployed", true},
                             {"program", d["program"]}, {"files", d["files"]}, {"draft", d["draft"]}};
             }
             std::string source = api_.programSource(plc, program);
             return json{{"_text", source}, {"source", source}, {"hash", sourceHash(source)}};
         }});

    add({"set_program", "Save and deploy a program",
         "Saves a Structured Text file and loads it at the next cycle boundary: into the tasks that run it, and into the "
         "running programs that use it as a library (listed in dependents). If it does not compile, it is kept as a draft: "
         "the previous version keeps running, also after a restart, and the result has the diagnostics (code, line, column, "
         "how to fix). Variables whose path and type definition stay keep their values; `reset` lists those that start "
         "again from their initial value. A program file holds one PROGRAM; a file without one is a library (its TYPEs, "
         "FUNCTIONs, FUNCTION_BLOCKs and constants are shared) and is checked as one. base_hash (from get_program) makes "
         "the save fail when someone else saved the file since.",
         schema({{"plc", kPlcArgument},
                 {"source", str("Structured Text source")},
                 {"program", str("Program file name (default: the main task's program, the first task's)")},
                 {"deploy", boolean("Load it now (default true); false keeps it as a draft")},
                 {"base_hash", str("The hash get_program gave for the version this one changes")}},
                {"source"}),
         false, false,
         [this](const json& a, const Principal& p) {
             std::string plc = plcArgument(api_, a);
             std::string program = optionalString(a, "program");
             if (program.empty()) program = primaryProgram(api_, plc, "");
             std::optional<std::string> base;
             if (std::string h = optionalString(a, "base_hash"); !h.empty()) base = h;
             json result;
             try {
                 result = api_.saveProgram(plc, program, requireString(a, "source"), a.value("deploy", true), base, &p);
             } catch (const ApiError& e) {
                 if (e.code != "program_changed") throw;
                 throw ApiError(e.status, e.code,
                                std::format("{}; nothing was saved. Read it again with get_program (its hash is now {}) and merge your change",
                                            e.what(), e.details.value("hash", json()).is_string() ? e.details["hash"].get<std::string>() : "none: it was deleted"),
                                e.details);
             }
             if (result["loaded"] == false) {
                 bool ownOk = true;
                 for (const json& d : result.value("diagnostics", json::array())) ownOk = ownOk && d.value("severity", "") != "error";
                 ownOk = ownOk && !result.contains("error");
                 for (const json& r : result.value("results", json::array())) {
                     if (!r.contains("program") && !r.value("ok", true)) ownOk = false;
                 }
                 std::string what = !ownOk ? (result.value("library", false)
                                                  ? "the library was saved as a draft: it does not compile, so the programs that use it keep their version:"
                                                  : "the program was saved as a draft: it failed to load, and the previous version keeps running (also after a restart):")
                                           : "the file was saved, but these programs that use it do not compile with it and keep running their previous version:";
                 throw ApiError(422, "load_failed", loadFailure(what, result));
             }
             return result;
         }});

    add({"check_program", "Compile a program",
         "Compiles Structured Text against the PLC's tags and library files without saving or running it, exactly as "
         "set_program would for the same program name: all diagnostics (errors and warnings with code, line, column and how "
         "to fix) and the memory it would use. A file without a PROGRAM that no task runs is checked as a library. For a "
         "program a task runs, online_change says what deploying it would do: kept (how many variables keep their values) "
         "and reset (those that would start from their initial value).",
         schema({{"plc", kPlcArgument}, {"source", str("Structured Text source")},
                 {"program", str("Program file it would be saved as (default: the main task's program, the first task's)")},
                 {"tags", array("Check against these tags instead of the PLC's (e.g. before set_tags)", kTagSchema)}},
                {"source"}),
         true, false,
         [this](const json& a, const Principal&) {
             std::string plc = plcArgument(api_, a);
             std::string program = optionalString(a, "program");
             if (program.empty()) program = primaryProgram(api_, plc, "");
             return api_.checkProgram(plc, program, requireString(a, "source"), a.contains("tags") ? a["tags"] : json(nullptr));
         }});

    add({"get_compiled", "Show the program as compiled",
         "The program the tasks run, as the compiler understood it: every expression fully parenthesised, literals typed and "
         "implicit conversions written out. Use it to confirm the evaluation order and types of a line. A draft that did not "
         "load is not shown here (check_program shows its diagnostics).",
         schema({{"plc", kPlcArgument}, {"program", str("Program name (default: the main task's program, the first task's)")}}), true, false,
         [this](const json& a, const Principal&) {
             std::string plc = plcArgument(api_, a);
             std::string program = optionalString(a, "program");
             if (program.empty()) program = primaryProgram(api_, plc, "");
             return api_.compiledProgram(plc, program);
         }});

    add({"update_task", "Change a task",
         "Changes a task's cycle time, program, priority, watchdog or enabled state. Only that task changes; it keeps its "
         "variables unless it switches to another program (then restarted_tasks lists it). A program that does not exist "
         "is refused unless create is true (it starts from an empty template); one that does not compile is refused, and "
         "the task keeps running its program.",
         schema({{"plc", kPlcArgument}, {"task", str("Task name")}, {"cycle_ms", integer("Cycle time (ms)", 1, 60000)},
                 {"program", str("Program name")}, {"enabled", boolean("Run this task in RUN mode")},
                 {"priority", integer("Real-time priority 1-98 (with the PLC's \"realtime\"); 0: normal scheduling", 0, 98)},
                 {"watchdog_ms", integer("Wall-clock limit per cycle in ms; 0: max(100, 10 x cycle_ms)", 0, 600000)},
                 {"create", boolean("Create the program from an empty template if it does not exist (default false)")},
                 {"config_version", kConfigVersionArgument}},
                {"task"}),
         false, false,
         [this](const json& a, const Principal& p) {
             json patch = json::object();
             for (const char* key : {"cycle_ms", "program", "enabled", "priority", "watchdog_ms"}) {
                 if (a.contains(key)) patch[key] = a[key];
             }
             ChangeOptions options = changeOptions(a);
             options.create = a.value("create", false);
             return api_.updateTask(plcArgument(api_, a), requireString(a, "task"), patch, options, &p);
         }});

    add({"get_variables", "Read program variables",
         "Current values of a task's program variables (including those inside function block instances, e.g. "
         "MAIN.FILL_TIMER.ET), taken between two cycles so they are consistent. At most 5000 are listed: then truncated is "
         "true, and a prefix narrows the list.",
         schema({{"plc", kPlcArgument}, {"task", str("Task name (default: the first task)")},
                 {"prefix", str("Only variables whose path starts with this, e.g. MAIN.FILL_TIMER")}}),
         true, false,
         [this](const json& a, const Principal&) {
             return api_.variables(plcArgument(api_, a), optionalString(a, "task"), optionalString(a, "prefix"));
         }});

    add({"set_variables", "Write program variables",
         "Writes program variables before the next cycle, e.g. {\"MAIN.SETPOINT\": 42.5, \"MAIN.MODE\": \"MODE#AUTO\", "
         "\"MAIN.LIMIT\": \"INT#5\"} (typed literals as get_variables shows them are accepted). All values are checked "
         "first; then all are written. A name without a dot belongs to the program.",
         schema({{"plc", kPlcArgument}, {"task", str("Task name (default: the first task)")},
                 {"values", object("Variable path -> value (number, true/false or an ST literal)")}},
                {"values"}),
         false, false,
         [this](const json& a, const Principal& p) {
             json body = {{"values", a.value("values", json::object())}};
             if (a.contains("task")) body["task"] = a["task"];
             return api_.setVariables(plcArgument(api_, a), body, &p);
         }});

    add({"get_logs", "Get the log", "SimplePLC's log: warnings, program errors, connections (debug).",
         schema({{"since", {{"type", "integer"}, {"minimum", 0}, {"description", "Only records after this seq (default 0)"}}},
                 {"level", {{"type", "string"}, {"enum", {"error", "warn", "info", "debug"}}, {"description", "Only this level and more severe"}}},
                 {"plc", str("Only this PLC")}, {"limit", integer("At most this many (default 200)", 1, 2000)}}),
         true, false,
         [this](const json& a, const Principal&) {
             return api_.logs(a.value("since", uint64_t{0}), optionalString(a, "level"), optionalString(a, "plc"),
                              a.value("limit", size_t{200}));
         }});

    add({"list_examples", "List example programs", "Bundled examples with their tags; setup_plc accepts their name as `example`.",
         schema(json::object()), true, false,
         [this](const json&, const Principal&) { return api_.examples(); }});

    add({"get_example", "Get an example", "An example's tags, tasks and Structured Text programs.",
         schema({{"name", str("Example name")}}, {"name"}), true, false,
         [this](const json& a, const Principal&) { return api_.example(requireString(a, "name")); }});

    add({"get_language_reference", "Structured Text reference",
         "The Structured Text SimplePLC accepts: program layout, tags (VAR_EXTERNAL), types, statements, operators, "
         "standard functions and blocks (TON, CTU, R_TRIG, ...), runtime faults and examples. Read it before writing a program.",
         schema(json::object()), true, false,
         [this](const json&, const Principal&) { return json(api_.languageReference()); }});

    add({"get_compliance", "IEC 61131-3 compliance statement",
         "Which IEC 61131-3 Structured Text features SimplePLC implements, how (implementation-dependent choices), and which "
         "it refuses, with the reason.",
         schema(json::object()), true, false,
         [this](const json&, const Principal&) { return json(api_.complianceStatement()); }});

    add({"get_events", "Who changed what",
         "The PLC's newest events, oldest first: program downloads and online changes (with how many values were kept and "
         "reset), draft saves, forces and releases, value writes, configuration changes, RUN/STOP/cold restart, faults, links "
         "going up or down, and changes of users and tokens. Each has seq, t_ms, event (program, force, release, write, config, "
         "mode, link, access), message and by ({kind: user, token or system, name}).",
         schema({{"plc", kPlcArgument}, {"limit", integer("At most this many, the newest (default 50)", 1, 500)}}), true, false,
         [this](const json& a, const Principal&) {
             return api_.events(plcArgument(api_, a), static_cast<size_t>(a.value("limit", 50)));
         }});

    add({"probe_device", "Probe a Modbus device",
         "Looks at a Modbus TCP device from the PLC's place in the network before you link it: its identification (vendor, "
         "product, revision; function 43/14) and up to 64 entries of each area from address 0 (fewer where the device refuses "
         "the range). It never writes. Each area stands on its own; latency_ms is how long the first answer took.",
         schema({{"plc", kPlcArgument}, {"host", str("Device address")}, {"port", integer("Modbus TCP port (default 502)", 1, 65535)},
                 {"unit", integer("Unit id (default 1)", 0, 255)}, {"timeout_ms", integer("Per request (default 500)", 10, 10000)}},
                {"host"}),
         true, false,
         [this](const json& a, const Principal&) {
             json body = json::object();
             for (const char* key : {"host", "port", "unit", "timeout_ms"}) {
                 if (a.contains(key)) body[key] = a[key];
             }
             return api_.probe(plcArgument(api_, a), body);
         }});

    if (fleet_) {
        add({"add_device", "Add a device to the fleet",
             "Adopts a SimplePLC device: give its web address and the pairing code it gives (its panel, or POST /api/v1/pairing "
             "there; 8 characters, valid 10 minutes, once), or an admin token of it. The fleet keeps its token and shows its PLC "
             "at once.",
             schema({{"url", str("The device's web address, e.g. http://10.0.0.5:8080 or tank")},
                     {"code", str("The pairing code from the device")},
                     {"token", str("An admin token of the device, instead of a code")}},
                    {"url"}),
             false, false,
             [this](const json& a, const Principal&) {
                 auto added = fleet_->addDevice(a);
                 if (!added) throw ApiError(added.error().status, added.error().code, added.error().message);
                 return *added;
             }});
        add({"remove_device", "Remove a device from the fleet",
             "Removes the device that runs PLC `plc` from the fleet, and revokes the fleet's token on it when it is reachable. "
             "Devices given on the fleet's command line cannot be removed here.",
             schema({{"plc", str("The PLC id of the device to remove")}}, {"plc"}), false, false,
             [this](const json& a, const Principal&) {
                 auto removed = fleet_->removeDevice(requireString(a, "plc"));
                 if (!removed) throw ApiError(removed.error().status, removed.error().code, removed.error().message);
                 return *removed;
             }});
    }

    json modbusTarget = {{"host", str("Device address (default 127.0.0.1)")},
                         {"port", integer("Modbus TCP port", 1, 65535)},
                         {"unit", integer("Unit id (default 1)", 0, 255)},
                         {"area", kAreaSchema},
                         {"address", integer("First address", 0, 65535)},
                         {"timeout_ms", integer("Timeout (default 1000)", 10, 10000)}};
    json readTarget = modbusTarget;
    readTarget["count"] = integer("Number of entries (default 1; at most 2000 bits or 125 registers)", 1, 2000);
    add({"modbus_read", "Modbus read (client)",
         "Reads from any Modbus TCP device, e.g. another PLC, to check a network from the outside.",
         schema(readTarget, {"port", "area", "address"}), true, false,
         [this](const json& a, const Principal&) { return api_.modbusRead(a); }});

    json writeTarget = modbusTarget;
    writeTarget["values"] = array("Values to write (coils: true/false, holding registers: numbers)", {{"type", {"boolean", "number"}}});
    add({"modbus_write", "Modbus write (client)", "Writes coils or holding registers of any Modbus TCP device.",
         schema(writeTarget, {"port", "area", "address", "values"}), false, false,
         [this](const json& a, const Principal&) { return api_.modbusWrite(a); }});

    // What a host may run without asking: from one table, so no tool is left optimistic
    for (Tool& tool : tools_) {
        Hints hints = hintsFor(tool.name, tool.readOnly);
        tool.destructive = hints.destructive;
        tool.idempotent = hints.idempotent;
    }
}

bool McpServer::supportsProtocol(std::string_view version) {
    return std::find(std::begin(kProtocolVersions), std::end(kProtocolVersions), version) != std::end(kProtocolVersions);
}

std::string McpServer::supportedProtocols() {
    std::string list;
    for (const char* v : kProtocolVersions) list += (list.empty() ? "" : ", ") + std::string(v);
    return list;
}

std::optional<json> McpServer::handle(const json& message, const Principal& principal) {
    // JSON-RPC 2.0 section 5.1: an id that cannot be read is answered as null
    json id = nullptr;
    if (message.is_object() && message.contains("id")) {
        const json& given = message["id"];
        if (given.is_string() || given.is_number() || given.is_null()) id = given;
    }
    auto error = [&](int code, const std::string& text) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", text}}}};
    };
    // Nothing a client sends may stop the PLC this server belongs to: every exception becomes an answer
    try {
        if (!message.is_object()) return error(kInvalidRequest, "Invalid Request: a message is a JSON object");
        if (message.contains("id") && !(message["id"].is_string() || message["id"].is_number() || message["id"].is_null())) {
            return error(kInvalidRequest, "Invalid Request: \"id\" must be a string or a number");
        }
        if (!message.contains("jsonrpc") || message["jsonrpc"] != "2.0") {
            return error(kInvalidRequest, "Invalid Request: \"jsonrpc\" must be \"2.0\"");
        }
        bool notification = !message.contains("id");
        if (!message.contains("method")) {
            // A response from the client (we send no requests, so there is nothing to match it to)
            if (message.contains("result") || message.contains("error")) return std::nullopt;
            return error(kInvalidRequest, "Invalid Request: \"method\" is missing");
        }
        if (!message["method"].is_string()) return error(kInvalidRequest, "Invalid Request: \"method\" must be a string");
        std::string method = message["method"].get<std::string>();
        if (method.starts_with("notifications/")) {
            if (notification) return std::nullopt;
            return error(kInvalidRequest, std::format("Invalid Request: {} is a notification, which has no id", method));
        }
        json params = json::object();
        if (message.contains("params")) {
            const json& p = message["params"];
            if (p.is_array()) return error(kInvalidParams, "Invalid params: MCP params are an object, not an array");
            if (!p.is_object()) return error(kInvalidRequest, "Invalid Request: \"params\" must be an object");
            params = p;
        }

        json result;
        if (method == "initialize") result = initialize(params);
        else if (method == "ping") result = json::object();
        else if (method == "tools/list") result = listTools();
        else if (method == "tools/call") result = callTool(params, principal);
        else if (method == "resources/list") result = listResources();
        else if (method == "resources/templates/list") result = listResourceTemplates();
        else if (method == "resources/read") result = readResource(params);
        else if (method == "prompts/list") result = listPrompts();
        else if (method == "prompts/get") result = getPrompt(params);
        else return notification ? std::nullopt : std::optional(error(kMethodNotFound, "Method not found: " + method));
        if (notification) return std::nullopt;
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    } catch (const RpcError& e) {
        return error(e.code, e.message);
    } catch (const ApiError& e) {
        return error(e.status == 404 ? kResourceNotFound : kInvalidParams, e.what());
    } catch (const std::exception& e) {
        return error(kInternalError, std::format("Internal error: {}", e.what()));
    } catch (...) {
        return error(kInternalError, "Internal error");
    }
}

namespace {

/// A string member of a request's params; -32602 naming it when it has another type.
std::string stringParam(const json& params, const char* key, bool required) {
    if (!params.contains(key) || params[key].is_null()) {
        if (required) throw RpcError{kInvalidParams, std::format("Invalid params: \"{}\" is required (a string)", key)};
        return {};
    }
    if (!params[key].is_string()) {
        throw RpcError{kInvalidParams, std::format("Invalid params: \"{}\" must be a string, not {}", key, describe(params[key]))};
    }
    return params[key].get<std::string>();
}

}  // namespace

json McpServer::initialize(const json& params) const {
    std::string requested = stringParam(params, "protocolVersion", false);
    std::string version = kProtocolVersions[0];
    for (const char* v : kProtocolVersions) {
        if (requested == v) version = v;
    }
    return {{"protocolVersion", version},
            {"capabilities", {{"tools", {{"listChanged", false}}},
                              {"resources", {{"listChanged", false}, {"subscribe", false}}},
                              {"prompts", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", "simpleplc"}, {"title", "SimplePLC"}, {"version", kVersion}}},
            {"instructions", kInstructions}};
}

json McpServer::listTools() const {
    json tools = json::array();
    for (const Tool& t : tools_) {
        tools.push_back({{"name", t.name},
                         {"title", t.title},
                         {"description", t.description},
                         {"inputSchema", t.inputSchema},
                         {"annotations", {{"title", t.title},
                                          {"readOnlyHint", t.readOnly},
                                          {"destructiveHint", t.destructive},
                                          {"idempotentHint", t.idempotent},
                                          {"openWorldHint", reachesOtherHosts(t)}}}});
    }
    return {{"tools", tools}};
}

json McpServer::callTool(const json& params, const Principal& principal) {
    std::string name = stringParam(params, "name", true);
    const Tool* tool = nullptr;
    for (const Tool& t : tools_) {
        if (t.name == name) tool = &t;
    }
    if (!tool) throw RpcError{kInvalidParams, "Unknown tool: " + name};
    json args = json::object();
    if (params.contains("arguments") && !params["arguments"].is_null()) {
        if (!params["arguments"].is_object()) {
            throw RpcError{kInvalidParams, std::format("Invalid params: \"arguments\" must be an object, not {}", describe(params["arguments"]))};
        }
        args = params["arguments"];
    }
    // As in the REST API: changing a PLC or connecting to other hosts needs an administrator
    if ((!tool->readOnly || reachesOtherHosts(*tool)) && principal.role != Role::Admin) {
        return toolError(std::format("{} needs an admin token; this token is read-only", name));
    }
    // Every argument checked against the tool's schema before anything runs: a missing one, a misspelt one
    // or one of the wrong type is refused by name (and never taken as "not given")
    std::vector<std::string> problems;
    checkValue(tool->inputSchema, args, "", problems);
    if (!problems.empty()) {
        std::string text = std::format("invalid arguments for {}:", name);
        for (const std::string& p : problems) text += "\n- " + p;
        return toolError(text);
    }
    if (fleet_) return callFleetTool(*tool, std::move(args), principal);
    try {
        return toolResult(tool->run(args, principal));
    } catch (const ApiError& e) {
        return toolError(e.what(), e.details);
    } catch (const json::exception& e) {
        return toolError(std::format("invalid arguments for {}: a value has the wrong type ({})", name, plainJsonError(e)));
    }
}

json McpServer::callFleetTool(const Tool& tool, json args, const Principal& principal) {
    if (tool.name == "list_plcs") return toolResult(fleet_->plcs());
    // Tools about one PLC run on its device; the rest (examples, docs, checks, the fleet's log,
    // Modbus from the fleet's place in the network) run here
    bool aboutOnePlc = tool.inputSchema["properties"].contains("plc") && tool.name != "get_logs" && tool.name != "remove_device";
    if (!aboutOnePlc) {
        try {
            return toolResult(tool.run(args, principal));
        } catch (const ApiError& e) {
            return toolError(e.what(), e.details);
        } catch (const json::exception& e) {
            return toolError(std::format("invalid arguments for {}: a value has the wrong type ({})", tool.name, plainJsonError(e)));
        }
    }
    std::string plc = optionalString(args, "plc");
    if (plc.empty()) {
        json plcs = fleet_->plcs();
        if (plcs.size() != 1) {
            std::string ids;
            for (const json& p : plcs) ids += (ids.empty() ? "" : ", ") + p.value("id", "");
            return toolError(std::format("'plc' is required in a fleet: one of {}", ids));
        }
        plc = plcs[0].value("id", "");
        args["plc"] = plc;
    }
    // A rename must not give two devices the same id: the fleet could not tell them apart
    if (tool.name == "update_plc" && args.contains("changes") && args["changes"].is_object() && args["changes"].contains("id") &&
        args["changes"]["id"].is_string() && args["changes"]["id"] != plc) {
        if (auto url = fleet_->deviceFor(plc)) {
            if (auto other = fleet_->otherDeviceWith(args["changes"]["id"].get<std::string>(), *url)) {
                return toolError(std::format("PLC id '{}' is already used by {} in this fleet", args["changes"]["id"].get<std::string>(), *other));
            }
        }
    }
    try {
        json result = forwardToDevice(plc, "tools/call", {{"name", tool.name}, {"arguments", args}}, &principal);
        // A change may rename the PLC or change its ports: let the fleet see it before the next call
        bool failed = !result.is_object() || !result.contains("isError") || !result["isError"].is_boolean() || result["isError"].get<bool>();
        if (!tool.readOnly && !failed) {
            if (auto url = fleet_->deviceFor(plc)) fleet_->refresh(*url);
        }
        return result;
    } catch (const RpcError& e) {
        return toolError(e.message);
    } catch (const json::exception& e) {
        return toolError(std::format("the device of PLC '{}' answered something unexpected: {}", plc, e.what()));
    }
}

json McpServer::forwardToDevice(const std::string& plc, const std::string& method, const json& params, const Principal* who) const {
    auto url = fleet_->deviceFor(plc);
    if (!url) throw RpcError{-32602, url.error().message};
    json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
    std::map<std::string, std::string> headers;
    if (who) headers[kOnBehalfHeader] = who->onBehalfOf.empty() ? who->name : std::format("{} via {}", who->onBehalfOf, who->name);
    auto reply = fleet_->send(*url, "POST", "/mcp", dumpJson(request), "application/json", headers);
    if (!reply) throw RpcError{-32603, std::format("PLC '{}': {}", plc, reply.error().message)};
    if (reply->status == 401 || reply->status == 403 || reply->status == 429) {
        throw RpcError{-32603, std::format("the device of PLC '{}' ({}) does not accept the fleet's token", plc, *url)};
    }
    if (reply->status != 200) {
        throw RpcError{-32603, std::format("the device of PLC '{}' ({}) answered HTTP {}", plc, *url, reply->status)};
    }
    json response = parseJson(reply->body);
    if (!response.is_object()) {
        throw RpcError{-32603, std::format("the device of PLC '{}' ({}) did not answer with JSON-RPC (its answer is not a JSON object)",
                                           plc, *url)};
    }
    if (response.contains("error")) {
        const json& e = response["error"];
        bool shaped = e.is_object() && e.contains("message") && e["message"].is_string();
        int code = e.is_object() && e.contains("code") && e["code"].is_number_integer() ? e["code"].get<int>() : -32603;
        throw RpcError{code, shaped ? e["message"].get<std::string>() : std::string("error from the device")};
    }
    if (!response.contains("result") || !response["result"].is_object()) {
        throw RpcError{-32603, std::format("the device of PLC '{}' ({}) sent no result", plc, *url)};
    }
    return response["result"];
}

json McpServer::plcTags(const std::string& plc) const {
    if (!fleet_) return api_.tags(plc);
    try {
        json result = forwardToDevice(plc, "resources/read", {{"uri", std::format("simpleplc://plcs/{}/tags", plc)}});
        return parseJson(result["contents"][0]["text"].get<std::string>());
    } catch (const std::exception&) {
        return json::array();  // the prompt still works without the current tags
    }
}

json McpServer::listResources() const {
    json resources = json::array({{{"uri", "simpleplc://docs/structured-text"},
                                   {"name", "structured-text"},
                                   {"title", "SimplePLC Structured Text"},
                                   {"description", "The language SimplePLC programs are written in"},
                                   {"mimeType", "text/markdown"}},
                                  {{"uri", "simpleplc://docs/st-compliance"},
                                   {"name", "st-compliance"},
                                   {"title", "IEC 61131-3 compliance statement"},
                                   {"description", "What SimplePLC implements of IEC 61131-3 Structured Text, and how"},
                                   {"mimeType", "text/markdown"}}});
    for (const Example& e : examples()) {
        resources.push_back({{"uri", "simpleplc://examples/" + e.name},
                             {"name", "example-" + e.name},
                             {"title", "Example: " + e.title},
                             {"description", e.description},
                             {"mimeType", "application/json"}});
    }
    for (const json& plc : fleet_ ? fleet_->plcs() : api_.listPlcs()) {
        std::string id = plc["id"].get<std::string>();
        resources.push_back({{"uri", std::format("simpleplc://plcs/{}/tags", id)},
                             {"name", id + "-tags"},
                             {"title", std::format("{}: tags and values", plc["name"].get<std::string>())},
                             {"mimeType", "application/json"}});
        if (fleet_) continue;  // the programs of a fleet's PLCs: see the resource template
        for (const json& program : api_.programs(id)) {
            std::string name = program["name"].get<std::string>();
            resources.push_back({{"uri", std::format("simpleplc://plcs/{}/programs/{}", id, name)},
                                 {"name", std::format("{}-{}", id, name)},
                                 {"title", std::format("{}: program {}", plc["name"].get<std::string>(), name)},
                                 {"mimeType", kStMime}});
        }
    }
    return {{"resources", resources}};
}

json McpServer::listResourceTemplates() const {
    return {{"resourceTemplates",
             json::array({{{"uriTemplate", "simpleplc://plcs/{plc}/programs/{program}"},
                           {"name", "program"},
                           {"title", "PLC program source"},
                           {"mimeType", kStMime}},
                          {{"uriTemplate", "simpleplc://plcs/{plc}/tags"},
                           {"name", "tags"},
                           {"title", "PLC tags with current values"},
                           {"mimeType", "application/json"}}})}};
}

json McpServer::readResource(const json& params) const {
    std::string uri = stringParam(params, "uri", true);
    auto contents = [&](const std::string& mime, const std::string& text) {
        return json{{"contents", json::array({{{"uri", uri}, {"mimeType", mime}, {"text", text}}})}};
    };
    constexpr std::string_view kPrefix = "simpleplc://";
    if (!uri.starts_with(kPrefix)) throw RpcError{kResourceNotFound, "Resource not found: " + uri};
    std::string path = uri.substr(kPrefix.size());

    try {
        if (path == "docs/structured-text") return contents("text/markdown", api_.languageReference());
        if (path == "docs/st-compliance") return contents("text/markdown", api_.complianceStatement());
        if (path.starts_with("examples/")) return contents("application/json", dumpJson(api_.example(path.substr(9)), 2));
        if (path.starts_with("plcs/")) {
            std::string rest = path.substr(5);
            size_t slash = rest.find('/');
            if (slash != std::string::npos) {
                std::string plc = rest.substr(0, slash);
                std::string what = rest.substr(slash + 1);
                if (fleet_) return forwardToDevice(plc, "resources/read", params);
                if (what == "tags") return contents("application/json", dumpJson(api_.tags(plc), 2));
                if (what.starts_with("programs/")) return contents(kStMime, api_.programSource(plc, what.substr(9)));
            }
        }
    } catch (const ApiError& e) {
        // No such example, PLC or program (MCP: -32002), e.g. a resource listed before a rename
        if (e.status == 404) throw RpcError{kResourceNotFound, std::format("Resource not found: {} ({})", uri, e.what())};
        throw;
    }
    throw RpcError{kResourceNotFound, "Resource not found: " + uri};
}

json McpServer::listPrompts() const {
    return {{"prompts",
             json::array({{{"name", "write_plc_program"},
                           {"title", "Write a PLC program"},
                           {"description", "Write, test, deploy and verify a Structured Text program for a PLC from a plain description"},
                           {"arguments", json::array({{{"name", "plc"}, {"description", "PLC id"}, {"required", true}},
                                                      {{"name", "behaviour"}, {"description", "What the program should do"}, {"required", true}}})}},
                          {{"name", "build_network"},
                           {"title", "Build a PLC network"},
                           {"description", "Program several PLC devices that talk Modbus TCP to each other, e.g. a plant and its controllers"},
                           {"arguments", json::array({{{"name", "scenario"}, {"description", "The plant or network to build"}, {"required", true}}})}}})}};
}

json McpServer::getPrompt(const json& params) const {
    std::string name = stringParam(params, "name", true);
    json args = json::object();
    if (params.contains("arguments") && !params["arguments"].is_null()) {
        if (!params["arguments"].is_object()) throw RpcError{kInvalidParams, "Invalid params: \"arguments\" must be an object"};
        args = params["arguments"];
        for (const auto& [key, value] : args.items()) {
            if (!value.is_string()) throw RpcError{kInvalidParams, std::format("Invalid params: argument '{}' must be a string", key)};
        }
    }
    // The prompts' required arguments (see listPrompts), refused when missing rather than filled with nothing.
    // Held in a variable: a range-for over a member of a temporary dangles before C++23's P2718 (GCC 13)
    const json prompts = listPrompts();
    for (const json& prompt : prompts["prompts"]) {
        if (prompt["name"] != name) continue;
        for (const json& argument : prompt["arguments"]) {
            std::string key = argument["name"].get<std::string>();
            if (argument.value("required", false) && (!args.contains(key) || args[key].get<std::string>().empty())) {
                throw RpcError{kInvalidParams, std::format("Invalid params: prompt '{}' needs the argument '{}' ({})", name, key,
                                                           argument["description"].get<std::string>())};
            }
        }
    }
    auto message = [](const std::string& text) {
        return json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", text}}}}});
    };
    if (name == "write_plc_program") {
        std::string plc = args.value("plc", "");
        std::string tags = plc.empty() ? "[]" : dumpJson(plcTags(plc), 2);
        return {{"description", "Write a PLC program"},
                {"messages", message(std::format(
                                 "Write a Structured Text program for SimplePLC PLC '{}'.\n\nRequired behaviour:\n{}\n\n"
                                 "Current tags (add any you need with set_tags first):\n{}\n\n"
                                 "Rules: one PROGRAM; declare the tags it uses in VAR_EXTERNAL with matching types; keep state in "
                                 "VAR (it persists between cycles); use TON/TOF/TP for timing and R_TRIG/F_TRIG for edges. Every tag "
                                 "that drives equipment should have a `substitute` value for faults.\n\n"
                                 "1. check_program until it compiles without errors; read the warnings too.\n"
                                 "2. set_program (deploy=false keeps a draft until you deploy it). Test on a PLC that is not "
                                 "driving equipment, or with external tools (a Modbus client, Factory I/O, an OPC UA client).\n"
                                 "3. read_values, write_values, force_values and get_variables to show each required behaviour, "
                                 "including timers and edge cases; release every force afterwards.\n"
                                 "Fix and repeat until it does.\n\nStructured Text reference:\n{}",
                                 plc, args.value("behaviour", ""), tags, api_.languageReference()))}};
    }
    if (name == "build_network") {
        return {{"description", "Build a PLC network"},
                {"messages", message(std::format(
                                 "Build this scenario as a network of SimplePLC devices:\n{}\n\n"
                                 "Each PLC is its own device (a container or a Raspberry Pi); list_plcs shows the ones you can "
                                 "reach. Decide which PLC controls which machine or part of the process, their tags and "
                                 "who polls whom: the PLC that polls (the Modbus TCP client) lists each device it polls (a Modbus "
                                 "TCP server) in its configuration's `links` (update_plc): host "
                                 "(in Docker: the service name), port, which of its tags it reads from which addresses and "
                                 "writes to which, and a status tag that is TRUE while the link works. Programs only use tags. "
                                 "Set each PLC up with setup_plc, verify the links from the outside with modbus_read and "
                                 "read_values, and summarise the network (clients, servers, endpoints) at the end.\n\n"
                                 "Structured Text reference:\n{}",
                                 args.value("scenario", ""), api_.languageReference()))}};
    }
    throw RpcError{-32602, "Unknown prompt: " + name};
}

}  // namespace simpleplc::web
