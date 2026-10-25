#include "app/config.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <tuple>

#include "app/log.h"
#include "net/socket.h"
#include "runtime/program_store.h"
#include "st/sema.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace simpleplc {
namespace {

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

/// Lower case without '_' and '-': "periodMs", "Period-MS" and "period_ms" are the same key to a reader.
std::string looseKey(std::string_view key) {
    std::string out;
    for (char c : key) {
        if (c == '_' || c == '-') continue;
        out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
}

/// What the length limits count, and which characters a text may hold.
enum class Text {
    Line,       ///< one line: names, ids, addresses (no control characters)
    Paragraph,  ///< descriptions: may also hold line breaks and tabs
};

/// UTF-8 characters of `s`, or a problem: invalid UTF-8, or characters that make a name read
/// differently from what it is (control characters, bidirectional overrides).
std::expected<size_t, std::string> countCharacters(std::string_view s, Text kind) {
    size_t count = 0;
    for (size_t i = 0; i < s.size(); count++) {
        auto byte = static_cast<unsigned char>(s[i]);
        size_t length = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xE ? 3 : (byte >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > s.size()) return std::unexpected("is not valid UTF-8 text");
        uint32_t cp = length == 1 ? byte : byte & (0x7F >> length);
        for (size_t k = 1; k < length; k++) {
            auto next = static_cast<unsigned char>(s[i + k]);
            if ((next & 0xC0) != 0x80) return std::unexpected("is not valid UTF-8 text");
            cp = (cp << 6) | (next & 0x3F);
        }
        i += length;
        bool lineBreak = cp == '\n' || cp == '\t' || cp == '\r';
        if ((cp < 0x20 && !(kind == Text::Paragraph && lineBreak)) || (cp >= 0x7F && cp <= 0x9F)) {
            return std::unexpected(std::format("must not hold control characters (U+{:04X}){}", cp,
                                               kind == Text::Line ? "; use one line of text" : ""));
        }
        if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x2028 || cp == 0x2029) {
            return std::unexpected(std::format("must not hold U+{:04X}, which changes how the text around it reads", cp));
        }
    }
    return count;
}

/// An address servers can listen on: an IP address of this device, "localhost", or empty/"*" for every interface.
bool isListenAddress(const std::string& s) {
    if (s.empty() || s == "*" || s == "localhost") return true;
    in_addr v4{};
    in6_addr v6{};
    return inet_pton(AF_INET, s.c_str(), &v4) == 1 || inet_pton(AF_INET6, s.c_str(), &v6) == 1;
}

constexpr const char* kListenHelp = "must be an IP address of this device: 0.0.0.0 for every interface, 127.0.0.1 "
                                    "for this device only, or one of its addresses";

/// Reads fields of one JSON object, collecting problems instead of throwing. Every key it reads (or
/// allows) is known; refuseUnknown() reports the others, so a misspelt key is never silently ignored.
class Reader {
public:
    Reader(const json& j, std::string path, std::vector<std::string>& problems)
        : j_(j), path_(std::move(path)), problems_(problems) {
        if (!j_.is_object()) problems_.push_back(std::format("{}: must be an object", path_));
    }

    bool ok() const { return j_.is_object(); }
    const json& raw() const { return j_; }
    std::string at(const char* key) const { return std::format("{}.{}", path_, key); }
    /// True when the object has `key` (null counts as absent).
    bool has(const char* key) { return find(key) != nullptr; }

    /// `maxLength` counts characters, not bytes.
    void string(const char* key, std::string& out, size_t maxLength = 256, Text kind = Text::Line) {
        const json* v = find(key);
        if (!v) return;
        if (!v->is_string()) return problem(key, "must be a string");
        auto s = v->get<std::string>();
        auto count = countCharacters(s, kind);
        if (!count) return problem(key, count.error());
        if (*count > maxLength) return problem(key, std::format("must be at most {} characters (it has {})", maxLength, *count));
        out = std::move(s);
    }

    /// A listen address (see isListenAddress).
    void listen(const char* key, std::string& out) {
        std::string value = out;
        size_t before = problems_.size();
        string(key, value, 128);
        if (problems_.size() != before) return;
        if (!isListenAddress(value)) return problem(key, std::format("'{}' {}", value, kListenHelp));
        out = std::move(value);
    }

    /// Reports every key that was neither read nor allowed, with the closest known key.
    void refuseUnknown() {
        if (!j_.is_object()) return;
        for (const auto& [key, value] : j_.items()) {
            if (std::find(known_.begin(), known_.end(), key) != known_.end()) continue;
            std::string best;
            size_t bestDistance = std::string::npos;
            for (const std::string& k : known_) {
                size_t d = looseKey(k) == looseKey(key) ? 0 : editDistance(key, k);
                if (d < bestDistance) bestDistance = d, best = k;
            }
            bool close = !best.empty() && (bestDistance <= std::max<size_t>(2, key.size() / 3) || best.starts_with(key) ||
                                           key.starts_with(best));
            problems_.push_back(close ? std::format("{}.{}: unknown key (did you mean {}?)", path_, key, best)
                                      : std::format("{}.{}: unknown key (known: {})", path_, key, knownList()));
        }
    }

    void integer(const char* key, int& out, int min, int max) {
        const json* v = find(key);
        if (!v) return;
        // 5.0 is a whole number too (JSON does not tell integers from other numbers)
        bool whole = v->is_number_integer() || (v->is_number_float() && std::trunc(v->get<double>()) == v->get<double>() &&
                                                std::abs(v->get<double>()) < 9.0e15);
        if (!whole) return problem(key, std::format("must be a whole number between {} and {}", min, max));
        auto n = v->is_number_float() ? static_cast<int64_t>(v->get<double>()) : v->get<int64_t>();
        if (v->is_number_unsigned() && v->get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)) n = INT64_MAX;
        if (n < min || n > max) return problem(key, std::format("must be between {} and {}", min, max));
        out = static_cast<int>(n);
    }

    void boolean(const char* key, bool& out) {
        const json* v = find(key);
        if (!v) return;
        if (!v->is_boolean()) return problem(key, "must be true or false");
        out = v->get<bool>();
    }

    const json* object(const char* key) {
        const json* v = find(key);
        if (v && !v->is_object()) {
            problem(key, "must be an object");
            return nullptr;
        }
        return v;
    }

    const json* array(const char* key) {
        const json* v = find(key);
        if (v && !v->is_array()) {
            problem(key, "must be an array");
            return nullptr;
        }
        return v;
    }

    void problem(const char* key, const std::string& message) {
        problems_.push_back(std::format("{}: {}", at(key), message));
    }

private:
    const json* find(const char* key) {
        if (std::find(known_.begin(), known_.end(), key) == known_.end()) known_.emplace_back(key);
        if (!j_.is_object()) return nullptr;
        auto it = j_.find(key);
        return it == j_.end() || it->is_null() ? nullptr : &*it;
    }

    std::string knownList() const {
        std::string list;
        for (const std::string& k : known_) list += (list.empty() ? "" : ", ") + k;
        return list;
    }

    const json& j_;
    std::string path_;
    std::vector<std::string>& problems_;
    std::vector<std::string> known_;
};

}  // namespace

std::string plcIdFromHostname(std::string_view host) {
    std::string id;
    for (char c : host.substr(0, host.find('.'))) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        id += ok ? c : '-';
    }
    if (id.empty()) return "plc";
    if (id[0] < 'a' || id[0] > 'z') id = "plc-" + id;  // container ids are hex and may start with a digit
    if (id.size() > 32) id.resize(32);
    return id;
}

bool isValidPlcId(std::string_view id) {
    if (id.empty() || id.size() > 32 || !(id[0] >= 'a' && id[0] <= 'z')) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

json toJson(const TagDef& tag) {
    json j = {{"name", tag.name},
              {"area", areaName(tag.area)},
              {"address", tag.address},
              {"type", tagTypeName(tag.type)}};
    if (!tag.description.empty()) j["description"] = tag.description;
    if (tag.substitute) j["substitute"] = tag.type == TagType::Bool ? json(*tag.substitute != 0) : json(*tag.substitute);
    if (tag.retain) j["retain"] = true;
    return j;
}

json toJson(const TaskConfig& task) {
    json j = {{"name", task.name}, {"program", task.program}, {"cycle_ms", task.cycleMs}, {"enabled", task.enabled}};
    if (task.priority) j["priority"] = task.priority;
    if (task.watchdogMs) j["watchdog_ms"] = task.watchdogMs;
    return j;
}

json toJson(const LinkConfig& link) {
    auto mappings = [](const std::vector<LinkMapping>& list) {
        json a = json::array();
        for (const LinkMapping& m : list) a.push_back({{"tag", m.tag}, {"area", areaName(m.area)}, {"address", m.address}});
        return a;
    };
    json j = {{"name", link.name}, {"host", link.host}, {"port", link.port}, {"unit", link.unit},
              {"period_ms", link.periodMs}, {"timeout_ms", link.timeoutMs},
              {"read", mappings(link.read)}, {"write", mappings(link.write)}};
    if (!link.statusTag.empty()) j["status_tag"] = link.statusTag;
    if (link.staleMs) j["stale_ms"] = link.staleMs;
    if (link.wordOrder != modbus::WordOrder::Abcd) j["word_order"] = modbus::wordOrderName(link.wordOrder);
    return j;
}

json toJson(const PlcConfig& plc) {
    json links = json::array();
    for (const auto& l : plc.links) links.push_back(toJson(l));
    json tasks = json::array();
    for (const auto& t : plc.tasks) tasks.push_back(toJson(t));
    json tags = json::array();
    for (const auto& t : plc.tags) tags.push_back(toJson(t));
    return {
        {"id", plc.id},
        {"name", plc.name},
        {"description", plc.description},
        {"image_size", plc.imageSize},
        {"autostart", plc.autostart},
        {"realtime", plc.realtime},
        {"device", {{"unit_id", plc.device.unitId}, {"vendor", plc.device.vendor},
                    {"product_code", plc.device.productCode}, {"model", plc.device.model}}},
        {"modbus", {{"enabled", plc.modbus.enabled}, {"listen", plc.modbus.listen},
                    {"port", plc.modbus.port}, {"max_connections", plc.modbus.maxConnections},
                    {"idle_timeout_s", plc.modbus.idleTimeoutS}}},
        {"opcua", {{"enabled", plc.opcua.enabled}, {"listen", plc.opcua.listen}, {"port", plc.opcua.port}}},
        {"tasks", tasks},
        {"tags", tags},
        {"links", links},
    };
}

json toJson(const Config& config) {
    return {{"version", 1},
            {"config_version", config.configVersion},
            {"web", {{"listen", config.web.listen}, {"port", config.web.port}}},
            {"log", {{"level", config.logLevel}}},
            {"plc", toJson(config.plc)}};
}

std::expected<TagDef, std::vector<std::string>> tagFromJson(const json& j, const std::string& path) {
    std::vector<std::string> problems;
    Reader r(j, path, problems);
    if (!r.ok()) return std::unexpected(problems);
    TagDef tag;
    r.string("name", tag.name, 64);
    std::string area, type;
    r.string("area", area, 32);
    r.string("type", type, 32);
    r.string("description", tag.description, 256, Text::Paragraph);
    const char* substituteKey = "substitute";
    if (!r.has(substituteKey) && r.raw().contains("safe")) substituteKey = "safe";  // the key earlier 2.0 builds wrote
    if (r.has(substituteKey)) {
        const json& s = j.at(substituteKey);
        if (s.is_boolean()) tag.substitute = s.get<bool>() ? 1.0 : 0.0;
        else if (s.is_number()) tag.substitute = s.get<double>();
        else problems.push_back(r.at(substituteKey) + ": must be a number, true or false (null or no substitute for none)");
    }
    r.boolean("retain", tag.retain);
    int address = -1;
    r.integer("address", address, 0, 65535);

    if (!r.has("name")) problems.push_back(r.at("name") + ": is required");
    if (!r.has("area")) {
        problems.push_back(r.at("area") + ": is required (coil, discrete_input, holding_register or input_register)");
    } else if (auto a = parseArea(area)) {
        tag.area = *a;
    } else if (!area.empty()) {
        problems.push_back(std::format("{}: '{}' is not one of coil, discrete_input, holding_register, input_register",
                                       r.at("area"), area));
    }
    if (!r.has("address")) problems.push_back(r.at("address") + ": is required");
    tag.address = static_cast<uint32_t>(std::max(address, 0));
    if (type.empty()) {
        tag.type = defaultTagType(tag.area);
    } else if (auto t = parseTagType(type)) {
        tag.type = *t;
    } else {
        problems.push_back(std::format("{}: '{}' is not one of bool, uint16, int16, uint32, int32, float32",
                                       r.at("type"), type));
    }
    // What GET .../tags adds to each tag: data, not configuration
    for (const char* key : {"value", "forced", "origin", "writable"}) {
        if (r.raw().contains(key)) {
            problems.push_back(std::format("{}.{}: is not part of a tag's configuration (it is shown with the tags; "
                                           "write values with write_values or PUT .../values)", path, key));
            r.has(key);
        }
    }
    r.refuseUnknown();
    if (!problems.empty()) return std::unexpected(problems);
    return tag;
}

std::expected<std::vector<TagDef>, std::vector<std::string>> tagsFromJson(const json& j, const std::string& path) {
    std::vector<std::string> problems;
    std::vector<TagDef> tags;
    if (!j.is_array()) return std::unexpected(std::vector<std::string>{path + ": must be an array"});
    for (size_t i = 0; i < j.size(); i++) {
        auto tag = tagFromJson(j[i], std::format("{}[{}]", path, i));
        if (tag) {
            tags.push_back(*tag);
        } else {
            problems.insert(problems.end(), tag.error().begin(), tag.error().end());
        }
    }
    if (!problems.empty()) return std::unexpected(problems);
    return tags;
}

std::expected<PlcConfig, std::vector<std::string>> plcFromJson(const json& j, const std::string& path) {
    std::vector<std::string> problems;
    Reader r(j, path, problems);
    if (!r.ok()) return std::unexpected(problems);

    PlcConfig plc;
    r.string("id", plc.id, 32);
    r.string("name", plc.name, 64);
    r.string("description", plc.description, 512, Text::Paragraph);
    r.integer("image_size", plc.imageSize, 1, 65536);
    r.boolean("autostart", plc.autostart);
    r.boolean("realtime", plc.realtime);
    // Configurations written by earlier 2.0 builds carry "clock": "real" (accepted and dropped). The simulated
    // clock is gone: a PLC set up for it is refused rather than started in real time behind its owner's back
    std::string clock = "real";
    r.string("clock", clock, 16);
    if (clock != "real") {
        problems.push_back(r.at("clock") + ": the simulated clock was removed (a PLC runs in real time); delete \"clock\" "
                           "from the configuration and test the program against external tools or another PLC");
    }
    if (plc.name.empty()) plc.name = plc.id;

    if (const json* d = r.object("device")) {
        Reader dr(*d, r.at("device"), problems);
        dr.integer("unit_id", plc.device.unitId, 0, 255);
        dr.string("vendor", plc.device.vendor, 64);
        dr.string("product_code", plc.device.productCode, 64);
        dr.string("model", plc.device.model, 64);
        dr.refuseUnknown();
    }
    if (const json* m = r.object("modbus")) {
        Reader mr(*m, r.at("modbus"), problems);
        mr.boolean("enabled", plc.modbus.enabled);
        mr.listen("listen", plc.modbus.listen);
        mr.integer("port", plc.modbus.port, 1, 65535);
        mr.integer("max_connections", plc.modbus.maxConnections, 1, 1000);
        mr.integer("idle_timeout_s", plc.modbus.idleTimeoutS, 0, 86400);
        mr.refuseUnknown();
    }
    if (const json* o = r.object("opcua")) {
        Reader orr(*o, r.at("opcua"), problems);
        orr.boolean("enabled", plc.opcua.enabled);
        orr.listen("listen", plc.opcua.listen);
        orr.integer("port", plc.opcua.port, 1, 65535);
        orr.refuseUnknown();
    }
    if (const json* tasks = r.array("tasks")) {
        plc.tasks.clear();
        for (size_t i = 0; i < tasks->size(); i++) {
            Reader tr((*tasks)[i], std::format("{}[{}]", r.at("tasks"), i), problems);
            TaskConfig task;
            tr.string("name", task.name, 64);
            tr.string("program", task.program, 64);
            tr.integer("cycle_ms", task.cycleMs, 1, 60000);
            if (!tr.has("cycle_ms") && tr.raw().contains("scan_ms")) tr.integer("scan_ms", task.cycleMs, 1, 60000);  // written by earlier 2.0 builds
            tr.boolean("enabled", task.enabled);
            tr.integer("priority", task.priority, 0, 98);
            tr.integer("watchdog_ms", task.watchdogMs, 0, 600000);
            tr.refuseUnknown();
            plc.tasks.push_back(task);
        }
    }
    if (const json* links = r.array("links")) {
        plc.links.clear();
        for (size_t i = 0; i < links->size(); i++) {
            std::string at = std::format("{}[{}]", r.at("links"), i);
            Reader lr((*links)[i], at, problems);
            LinkConfig link;
            lr.string("name", link.name, 64);
            lr.string("host", link.host, 253);
            lr.integer("port", link.port, 1, 65535);
            lr.integer("unit", link.unit, 0, 255);
            lr.integer("period_ms", link.periodMs, 10, 60000);
            lr.integer("timeout_ms", link.timeoutMs, 10, 10000);
            lr.string("status_tag", link.statusTag, 64);
            lr.integer("stale_ms", link.staleMs, 0, 600000);
            std::string wordOrder(modbus::wordOrderName(link.wordOrder));
            lr.string("word_order", wordOrder, 8);
            if (auto order = modbus::parseWordOrder(wordOrder)) link.wordOrder = *order;
            else lr.problem("word_order", "must be \"abcd\" (32-bit values high word first) or \"cdab\" (low word first)");
            for (const char* key : {"read", "write"}) {
                const json* list = lr.array(key);
                if (!list) continue;
                for (size_t k = 0; k < list->size(); k++) {
                    Reader mr((*list)[k], std::format("{}.{}[{}]", at, key, k), problems);
                    LinkMapping m;
                    mr.string("tag", m.tag, 64);
                    std::string area;
                    mr.string("area", area, 32);
                    if (auto a = parseArea(area)) m.area = *a;
                    else if (mr.ok()) mr.problem("area", "must be coil, discrete_input, holding_register or input_register");
                    mr.integer("address", m.address, 0, 65535);
                    mr.refuseUnknown();
                    (std::string_view(key) == "read" ? link.read : link.write).push_back(m);
                }
            }
            lr.refuseUnknown();
            plc.links.push_back(link);
        }
    }
    if (const json* tags = r.array("tags")) {
        auto parsed = tagsFromJson(*tags, r.at("tags"));
        if (parsed) {
            plc.tags = *parsed;
        } else {
            problems.insert(problems.end(), parsed.error().begin(), parsed.error().end());
        }
    }
    r.refuseUnknown();
    if (!problems.empty()) return std::unexpected(problems);
    return plc;
}

std::vector<std::string> validatePlc(const PlcConfig& plc) {
    std::vector<std::string> problems;
    if (!isValidPlcId(plc.id)) {
        problems.push_back(std::format("PLC id '{}' is invalid: use lowercase letters, digits, - and _, "
                                       "starting with a letter, at most 32 characters", plc.id));
    }
    if (plc.imageSize < 1 || plc.imageSize > 65536) problems.push_back("image_size must be 1-65536");
    if (plc.tasks.empty()) problems.push_back("a PLC needs at least one task");
    if (plc.tasks.size() > 8) problems.push_back("a PLC can have at most 8 tasks");
    std::set<std::string> taskNames;
    for (const auto& task : plc.tasks) {
        if (!isValidName(task.name)) problems.push_back(std::format("task name '{}' is invalid", task.name));
        if (!taskNames.insert(task.name).second) problems.push_back(std::format("task name '{}' is used twice", task.name));
        if (!runtime::ProgramStore::isValidName(task.program)) {
            problems.push_back(std::format("task '{}': program name '{}' is invalid (letters, digits, _ and -)",
                                           task.name, task.program));
        }
        if (task.cycleMs < 1 || task.cycleMs > 60000) {
            problems.push_back(std::format("task '{}': cycle_ms must be 1-60000", task.name));
        }
    }
    for (const auto& [what, server] : {std::pair{"Modbus", &plc.modbus.listen}, std::pair{"OPC UA", &plc.opcua.listen}}) {
        if (!isListenAddress(*server)) problems.push_back(std::format("{} listen address '{}' {}", what, *server, kListenHelp));
    }
    for (const auto& [what, port] : {std::pair{"Modbus", plc.modbus.port}, std::pair{"OPC UA", plc.opcua.port}}) {
        if (port < 1 || port > 65535) problems.push_back(std::format("{} port {} must be 1-65535", what, port));
    }
    if (plc.modbus.enabled && plc.opcua.enabled && plc.modbus.port == plc.opcua.port) {
        problems.push_back(std::format("Modbus and OPC UA cannot both use port {}", plc.modbus.port));
    }
    auto tagProblems = validateTags(plc.tags, static_cast<uint32_t>(plc.imageSize));
    problems.insert(problems.end(), tagProblems.begin(), tagProblems.end());
    // Programs bind tags by name: a tag no program can name is a configuration mistake
    for (const TagDef& tag : plc.tags) {
        if (!isValidName(tag.name) || tag.name.find("__") != std::string::npos || tag.name.back() == '_') continue;  // said above
        if (std::string problem = st::nameProblem(tag.name); !problem.empty()) {
            problems.push_back(std::format("tag name '{}' cannot be used in a program: {}; choose another name, e.g. '{}_tag'",
                                           tag.name, problem, tag.name));
        }
    }
    auto linkProblems = validateLinks(plc);
    problems.insert(problems.end(), linkProblems.begin(), linkProblems.end());
    return problems;
}

std::vector<std::string> validateLinks(const PlcConfig& plc) {
    std::vector<std::string> problems;
    if (plc.links.size() > 16) problems.push_back("a PLC can have at most 16 links");
    auto findTag = [&](const std::string& name) -> const TagDef* {
        for (const TagDef& t : plc.tags) {
            if (t.name == name) return &t;
        }
        return nullptr;
    };
    std::set<std::string> names;
    std::map<std::string, std::string> writers;  // tag -> the link that writes it
    // A device register written by two mappings would take turns between their values
    struct RemoteWriter {
        std::string link, tag;
    };
    std::map<std::tuple<std::string, int, int, Area, int>, RemoteWriter> remote;  // (host, port, unit, area, address)
    for (const LinkConfig& link : plc.links) {
        std::string at = std::format("link '{}'", link.name);
        if (!isValidName(link.name)) problems.push_back(std::format("link name '{}' is invalid", link.name));
        if (!names.insert(link.name).second) problems.push_back(std::format("link name '{}' is used twice", link.name));
        if (link.host.empty()) problems.push_back(at + ": host is missing");
        if (link.port < 1 || link.port > 65535) problems.push_back(at + ": port must be 1-65535");
        if (link.unit < 0 || link.unit > 255) problems.push_back(at + ": unit must be 0-255");
        if (link.periodMs < 10 || link.periodMs > 60000) problems.push_back(at + ": period_ms must be 10-60000");
        if (link.timeoutMs < 10 || link.timeoutMs > 10000) problems.push_back(at + ": timeout_ms must be 10-10000");
        if (link.staleMs > 0 && link.staleMs < link.periodMs) {
            problems.push_back(std::format("{}: stale_ms ({}) must be at least period_ms ({}): the status would go FALSE between "
                                           "two cycles that both succeed; leave it out for 3 × period_ms + timeout_ms",
                                           at, link.staleMs, link.periodMs));
        }
        if (link.read.empty() && link.write.empty()) problems.push_back(at + ": give the tags to read or write");
        if (link.read.size() + link.write.size() > 256) problems.push_back(at + ": at most 256 tags per link");
        auto claim = [&](const std::string& tag, const std::string& what) {
            auto [it, inserted] = writers.emplace(tag, link.name);
            if (!inserted) {
                problems.push_back(std::format("{}: tag '{}' ({}) is already written by link '{}'; a tag has one writer",
                                               at, tag, what, it->second));
            }
        };
        if (!link.statusTag.empty()) {
            const TagDef* t = findTag(link.statusTag);
            if (!t) problems.push_back(std::format("{}: status_tag '{}' is not a tag", at, link.statusTag));
            else if (t->type != TagType::Bool) problems.push_back(std::format("{}: status_tag '{}' must be a bool tag", at, link.statusTag));
            else if (t->area != Area::DiscreteInput) {
                // Programs use it as an interlock: no Modbus or OPC UA client may make it say TRUE
                problems.push_back(std::format("{}: status_tag '{}' is in the {} area, which Modbus and OPC UA clients can write; "
                                               "put it in discrete_input, so that only the link sets it",
                                               at, link.statusTag, areaName(t->area)));
            } else {
                claim(link.statusTag, "status");
            }
        }
        for (int dir = 0; dir < 2; dir++) {
            const auto& list = dir == 0 ? link.read : link.write;
            const char* key = dir == 0 ? "read" : "write";
            for (const LinkMapping& m : list) {
                const TagDef* t = findTag(m.tag);
                if (!t) {
                    problems.push_back(std::format("{}: {} tag '{}' is not a tag", at, key, m.tag));
                    continue;
                }
                bool bitTag = t->type == TagType::Bool;
                if (bitTag != isBitArea(m.area)) {
                    problems.push_back(std::format("{}: tag '{}' is {}; it maps to {}", at, m.tag, tagTypeName(t->type),
                                                   bitTag ? "coils or discrete inputs" : "holding or input registers"));
                    continue;
                }
                if (dir == 1 && m.area != Area::Coil && m.area != Area::HoldingRegister) {
                    problems.push_back(std::format("{}: tag '{}' is written to the device, so its area must be coil or "
                                                   "holding_register", at, m.tag));
                }
                if (m.address < 0 || static_cast<uint32_t>(m.address) + tagWidth(t->type) > 65536) {
                    problems.push_back(std::format("{}: tag '{}' at address {} does not fit (0-65535)", at, m.tag, m.address));
                }
                if (dir == 0) claim(m.tag, "read from the device");
                if (dir == 1) {
                    std::string host = link.host;
                    std::transform(host.begin(), host.end(), host.begin(),
                                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
                    for (uint32_t w = 0; w < tagWidth(t->type); w++) {
                        int address = m.address + static_cast<int>(w);
                        auto [it, inserted] = remote.emplace(std::tuple(host, link.port, link.unit, m.area, address),
                                                             RemoteWriter{link.name, m.tag});
                        if (!inserted) {
                            problems.push_back(std::format(
                                "{}: tag '{}' writes {} {} of {}:{} unit {}, which link '{}' already writes (tag '{}'); a device "
                                "register written twice takes turns between the values: write it from one tag",
                                at, m.tag, areaName(m.area), address, link.host, link.port, link.unit, it->second.link, it->second.tag));
                            break;
                        }
                    }
                }
            }
        }
    }
    return problems;
}

std::vector<std::string> validateConfig(const Config& config, int webPortInUse) {
    std::vector<std::string> problems;
    const PlcConfig& plc = config.plc;
    for (const auto& p : validatePlc(plc)) problems.push_back(std::format("plc: {}", p));
    if (!isListenAddress(config.web.listen)) problems.push_back(std::format("web.listen '{}' {}", config.web.listen, kListenHelp));
    // Modbus against OPC UA is validatePlc's (reported once); here each against the web panel: its configured
    // port, and the port it actually listens on (another one after a fallback, or given on the command line)
    for (const auto& [what, enabled, port] : {std::tuple{"Modbus", plc.modbus.enabled, plc.modbus.port},
                                              std::tuple{"OPC UA", plc.opcua.enabled, plc.opcua.port}}) {
        if (!enabled) continue;
        if (port == config.web.port) {
            problems.push_back(std::format("port {} is used by both the web panel and {}", port, what));
        } else if (port == webPortInUse) {
            problems.push_back(std::format("port {} is used by both the web panel (it listens there now) and {}", port, what));
        }
    }
    if (!log::parseLevel(config.logLevel)) {
        problems.push_back(std::format("log.level '{}' must be error, warn, info or debug", config.logLevel));
    }
    return problems;
}

std::expected<Config, std::vector<std::string>> configFromJson(const json& j) {
    std::vector<std::string> problems;
    Reader r(j, "config", problems);
    if (!r.ok()) return std::unexpected(problems);
    Config config;
    int version = 1;
    r.integer("version", version, 1, 1);
    if (r.has("config_version")) {
        const json& v = j.at("config_version");
        if (v.is_number_unsigned()) config.configVersion = v.get<uint64_t>();
        else r.problem("config_version", "must be a whole number, 0 or more");
    }
    if (const json* w = r.object("web")) {
        Reader wr(*w, "web", problems);
        wr.listen("listen", config.web.listen);
        wr.integer("port", config.web.port, 1, 65535);
        wr.refuseUnknown();
    }
    if (const json* l = r.object("log")) {
        Reader lr(*l, "log", problems);
        lr.string("level", config.logLevel, 16);
        lr.refuseUnknown();
    }
    if (r.has("plcs")) {
        // SimplePLC 1.0 previews held several PLCs per process; now each process is one PLC
        problems.push_back("config.plcs: SimplePLC runs one PLC per process now; move one PLC to \"plc\" and its "
                           "programs from plcs/<id>/programs/ to programs/, and run the others as separate "
                           "processes or containers");
    } else if (const json* p = r.object("plc")) {
        auto plc = plcFromJson(*p, "plc");
        if (plc) {
            config.plc = *plc;
        } else {
            problems.insert(problems.end(), plc.error().begin(), plc.error().end());
        }
    } else {
        problems.push_back("config: \"plc\" is missing");
    }
    r.refuseUnknown();
    // Check what could be parsed too, so one run reports every problem
    auto invalid = validateConfig(config);
    problems.insert(problems.end(), invalid.begin(), invalid.end());
    if (!problems.empty()) return std::unexpected(problems);
    return config;
}

std::expected<Config, std::vector<std::string>> loadConfig(const std::filesystem::path& path) {
    auto text = runtime::readFile(path);
    if (!text) return std::unexpected(std::vector<std::string>{text.error()});
    json j = json::parse(*text, nullptr, false, true);
    if (j.is_discarded()) {
        return std::unexpected(std::vector<std::string>{std::format("{} is not valid JSON", path.string())});
    }
    return configFromJson(j);
}

std::expected<void, std::string> saveConfig(const std::filesystem::path& path, const Config& config) {
    return runtime::writeFileAtomic(path, toJson(config).dump(2) + "\n");
}


}  // namespace simpleplc
