#pragma once
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tag.h"
#include "modbus/links.h"

namespace simpleplc {

using json = nlohmann::json;

struct DeviceConfig {
    int unitId = 1;
    std::string vendor = "SimplePLC";
    std::string productCode = "SPLC";
    std::string model = "Soft PLC";

    bool operator==(const DeviceConfig&) const = default;
};

struct ModbusConfig {
    bool enabled = true;
    std::string listen = "0.0.0.0";
    int port = 502;              ///< 0 = pick a free port when the PLC is created
    int maxConnections = 16;
    int idleTimeoutS = 60;       ///< 0 = never close idle connections

    bool operator==(const ModbusConfig&) const = default;
};

struct OpcUaConfig {
    bool enabled = true;
    std::string listen = "0.0.0.0";
    int port = 4840;             ///< 0 = pick a free port when the PLC is created

    bool operator==(const OpcUaConfig&) const = default;
};

struct TaskConfig {
    std::string name = "main";
    std::string program = "main";
    int cycleMs = 10;
    bool enabled = true;
    int priority = 0;       ///< 1–98 real-time priority (with "realtime"); 0: normal scheduling
    int watchdogMs = 0;     ///< wall-clock limit per cycle; 0: max(100 ms, 10 × cycle_ms)

    bool operator==(const TaskConfig&) const = default;
};

using modbus::LinkConfig;
using modbus::LinkMapping;

struct PlcConfig {
    std::string id;
    std::string name;
    std::string description;
    int imageSize = 1024;
    bool autostart = true;       ///< start the program when SimplePLC starts
    bool realtime = false;       ///< real-time scheduling for tasks with a priority (Linux SCHED_FIFO, needs CAP_SYS_NICE; macOS time-constraint policy)
    DeviceConfig device;
    ModbusConfig modbus;
    OpcUaConfig opcua;
    std::vector<TaskConfig> tasks{TaskConfig{}};
    std::vector<TagDef> tags;
    std::vector<LinkConfig> links;

    bool operator==(const PlcConfig&) const = default;
};

struct WebConfig {
    std::string listen = "0.0.0.0";
    int port = 8080;

    bool operator==(const WebConfig&) const = default;
};

/// A data directory's configuration: one device, one PLC.
struct Config {
    WebConfig web;
    std::string logLevel = "info";
    PlcConfig plc;
    /// Grows by 1 with every applied change of the PLC's configuration, so a change based on an
    /// older version can be refused (optimistic concurrency: If-Match, MCP config_version).
    uint64_t configVersion = 0;

    bool operator==(const Config&) const = default;
};

/// PLC ids: lowercase letters, digits, '-' and '_', starting with a letter, 1-32 characters.
bool isValidPlcId(std::string_view id);

json toJson(const TagDef& tag);
json toJson(const TaskConfig& task);
json toJson(const LinkConfig& link);
json toJson(const PlcConfig& plc);
json toJson(const Config& config);

/// Parses a tag; problems are prefixed with `path`. As everywhere in the configuration, unknown keys
/// are refused (naming the closest known key), lengths count characters, and names hold no control characters.
std::expected<TagDef, std::vector<std::string>> tagFromJson(const json& j, const std::string& path = "tag");
std::expected<std::vector<TagDef>, std::vector<std::string>> tagsFromJson(const json& j, const std::string& path = "tags");
/// Parses a PLC; missing fields get defaults. Does not check the ports against the web panel.
std::expected<PlcConfig, std::vector<std::string>> plcFromJson(const json& j, const std::string& path = "plc");
std::expected<Config, std::vector<std::string>> configFromJson(const json& j);

/// Problems with one PLC on its own; empty when valid.
std::vector<std::string> validatePlc(const PlcConfig& plc);
/// Problems with the PLC's links (their tags must exist; a tag has at most one link writing it).
std::vector<std::string> validateLinks(const PlcConfig& plc);
/// Problems with the PLC, and ports used twice (including the web panel's configured port and
/// `webPortInUse`, the port it actually listens on; 0: not known).
std::vector<std::string> validateConfig(const Config& config, int webPortInUse = 0);

std::expected<Config, std::vector<std::string>> loadConfig(const std::filesystem::path& path);
std::expected<void, std::string> saveConfig(const std::filesystem::path& path, const Config& config);

/// A valid PLC id made from a host name ("Tank-1.local" -> "tank-1", "3f2a9c" -> "plc-3f2a9c").
std::string plcIdFromHostname(std::string_view host);


}  // namespace simpleplc
