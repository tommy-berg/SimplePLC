#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/engine.h"
#include "web/auth.h"

namespace simpleplc::web {

using json = nlohmann::json;

/// Deepest nesting accepted in request bodies: copying a JSON value recurses, and a body of
/// 100 000 nested arrays overflowed a worker thread's stack.
constexpr int kMaxJsonDepth = 64;

/// Parses a request; invalid or too deeply nested JSON gives a discarded value.
json parseJson(std::string_view text);
/// Serialises a response. Program output can hold any bytes: invalid UTF-8 becomes U+FFFD
/// instead of an exception that would fail the whole response.
std::string dumpJson(const json& value, int indent = -1);

/// An error for the API caller: HTTP status, stable code, readable message.
struct ApiError : std::runtime_error {
    ApiError(int httpStatus, std::string errorCode, const std::string& message, json errorDetails = nullptr)
        : std::runtime_error(message), status(httpStatus), code(std::move(errorCode)), details(std::move(errorDetails)) {}

    int status;
    std::string code;  ///< "not_found", "invalid", "conflict", "forbidden", ...
    json details;
};

/// Conditions of a configuration change (REST: If-Match, ?force=1, "create": true; MCP: config_version,
/// force and create).
struct ChangeOptions {
    /// Refuse the change (412 config_changed) unless the configuration still has this version.
    std::optional<uint64_t> ifVersion;
    /// Apply a tag change although a running program no longer compiles with it (the PLC then faults).
    bool force = false;
    /// A task may name a program that does not exist: it starts from an empty template.
    bool create = false;
};
using ConfigChange = ChangeOptions;

/// The SHA-256 of a program's source in hex: its version for If-Match and ETag.
std::string sourceHash(std::string_view source);

/**
 * Every operation of the REST API and the MCP server, in JSON terms.
 *
 * Both front ends call these methods, so they always offer the same features
 * with the same validation. Methods throw ApiError for problems the caller can
 * fix. Thread-safe.
 */
class ApiService {
public:
    /// `engine` is null in a fleet, which has no PLC of its own: PLC calls go to the devices instead.
    ApiService(Engine* engine, Auth& auth);

    void setWebEndpoint(std::string listen, int port);

    // System
    json status() const;
    json logs(uint64_t since, const std::string& level, const std::string& plc, size_t limit) const;
    json settings() const;
    json updateSettings(const json& body, const Principal* by = nullptr);
    json examples() const;
    json example(const std::string& name) const;
    /// The Structured Text reference (docs/structured-text.md) and the compliance statement.
    std::string languageReference() const;
    std::string complianceStatement() const;

    // The PLC (one per device)
    json listPlcs() const;
    /// The PLC with its configuration and "config_version" (the version to give back with a change).
    json getPlc(const std::string& id) const;
    /// Sets the PLC up from scratch (optionally from an example); keeps its id and ports unless given.
    json setupPlc(const std::string& id, const json& body, const ChangeOptions& options = {}, const Principal* by = nullptr);
    /// A JSON merge patch; {"create": true} in it is ChangeOptions::create. The answer has "restarted_tasks".
    json updatePlc(const std::string& id, const json& patch, const ChangeOptions& options = {}, const Principal* by = nullptr);
    json runPlc(const std::string& id, const Principal* by = nullptr);
    /// The device the PLC runs on (model, OS, CPU, memory, storage, network), like a device manager.
    json device(const std::string& id) const;
    json stopPlc(const std::string& id, const Principal* by = nullptr);
    json restartPlc(const std::string& id, const Principal* by = nullptr);

    // Tags and values
    json tags(const std::string& id) const;
    // A tag change that would stop a running program (it no longer compiles) is refused with 409
    // "tag_in_use" and details.programs, unless forced
    json setTags(const std::string& id, const json& tags, const ChangeOptions& options = {}, const Principal* by = nullptr);
    json addTag(const std::string& id, const json& tag, const ChangeOptions& options = {}, const Principal* by = nullptr);
    json updateTag(const std::string& id, const std::string& name, const json& patch, const ChangeOptions& options = {},
                   const Principal* by = nullptr);
    void deleteTag(const std::string& id, const std::string& name, const ChangeOptions& options = {}, const Principal* by = nullptr);
    json values(const std::string& id, const std::vector<std::string>& names) const;
    json writeValues(const std::string& id, const json& values, const Principal* by = nullptr);
    json readArea(const std::string& id, const std::string& area, int start, int count) const;
    json writeArea(const std::string& id, const std::string& area, const json& body, const Principal* by = nullptr);
    json forces(const std::string& id) const;
    json force(const std::string& id, const json& body, const Principal* by = nullptr);
    json release(const std::string& id, const json& body, const Principal* by = nullptr);

    // Tasks and programs
    json tasks(const std::string& id) const;
    /// {"tasks": [...], "restarted_tasks": [...]}; {"create": true} in the patch is ChangeOptions::create.
    json updateTask(const std::string& id, const std::string& task, const json& patch, const ChangeOptions& options = {},
                    const Principal* by = nullptr);
    /// Program variables with their values (monitoring); `prefix` filters paths.
    json variables(const std::string& id, const std::string& task, const std::string& prefix) const;
    /// Writes program variables before the next cycle: {"task": "main", "values": {"MAIN.X": "42", …}}.
    json setVariables(const std::string& id, const json& body);
    json programs(const std::string& id) const;
    std::string programSource(const std::string& id, const std::string& name) const;
    /// Saves (and with `deploy` loads) a program file. `ifMatch`: the sourceHash of the version the caller
    /// changed; another one answers 412 "program_changed".
    json saveProgram(const std::string& id, const std::string& name, const std::string& source, bool deploy,
                     const std::optional<std::string>& ifMatch = std::nullopt, const Principal* by = nullptr);
    /// The files the tasks run a program with (what a restart runs): {program, files: [{name, source, hash}], draft}.
    json deployedProgram(const std::string& id, const std::string& name) const;
    /// The main task's program (the first task's): the default program of the tools.
    std::string mainProgram(const std::string& id) const;
    /// Compiles `source` as program `name` with the PLC's tags and libraries, without saving it.
    json checkProgram(const std::string& id, const std::string& name, const std::string& source,
                      const json& tags = nullptr) const;
    /// The program "as compiled" (listing) and its last diagnostics.
    json compiledProgram(const std::string& id, const std::string& name) const;
    void deleteProgram(const std::string& id, const std::string& name);
    json programHistory(const std::string& id, const std::string& name) const;
    std::string programVersion(const std::string& id, const std::string& name, const std::string& version) const;
    json restoreProgram(const std::string& id, const std::string& name, const std::string& version, bool deploy,
                        const Principal* by = nullptr);

    // Users and tokens
    json users() const;
    json addUser(const json& body, const Principal* by = nullptr);
    /// Removes a user and revokes the API tokens they created: {"deleted", "revoked_tokens"}.
    json removeUser(const std::string& name, const Principal& by);
    /// {"password", "current_password"}: changing your own password needs the current one; an
    /// administrator resetting another user's does not.
    json setPassword(const std::string& name, const json& body, const Principal& by);
    json tokens() const;
    json createToken(const json& body, const Principal& by);
    void revokeToken(const std::string& id, const Principal* by = nullptr);

    // Modbus client, for checking devices from the outside
    json modbusRead(const json& body) const;
    json modbusWrite(const json& body) const;

    // Events, devices and adoption (contracts R1, R4, R6)
    /// setVariables and deleteProgram, recorded as made by `by`.
    json setVariables(const std::string& id, const json& body, const Principal* by);
    void deleteProgram(const std::string& id, const std::string& name, const Principal* by);
    /// The newest events (changes and changes of state, with their authors), oldest first: {"events": [...]}.
    json events(const std::string& id, size_t limit) const;
    /// Reads up to 64 entries of every area of a device from address 0, and its identification; never writes.
    json probe(const std::string& id, const json& body) const;
    /// Public: what this is, for a fleet adopting it ({product, version, plc, needs_setup}).
    json hello() const;
    /// A new pairing code for a fleet (replaces the one before): {code, expires_ms}.
    json pairingCode(const Principal* by = nullptr);
    /// Public: trades a pairing code for an admin token named "fleet:<fleet>": {token, plc}.
    json pair(const json& body, const std::string& clientAddress);
    /// A fleet removing this device revokes the token it signs in with (`bearer`).
    json unpair(const std::string& bearer, const Principal* by = nullptr);

    /// What one live stream has sent so far (streamPlc): the tag table it knows and every tag's raw words,
    /// origin and forced state as last sent. A poll compares against these, so it builds JSON only for
    /// what changed, and allocates nothing while nothing changes.
    struct LiveState {
        bool first = true;
        std::shared_ptr<const ProcessImage> image;  ///< the image the words below were read from
        uint64_t version = 0;        ///< image version (values) at the last poll
        uint64_t tagsVersion = 0;
        uint64_t forcesVersion = 0;
        std::vector<TagDef> tags;
        std::vector<Slot> slots;
        std::vector<uint16_t> words, nextWords;      ///< every tag's words, consecutively (readSlots order)
        std::vector<Origin> origins, nextOrigins;    ///< per tag
        std::vector<uint8_t> forced, nextForced;     ///< per tag
    };
    /// The live stream's next "values" event ({"full", "values", "origins", "forced"}), or none when nothing
    /// the stream shows has changed since `state` (which it updates). The first call and every change of the
    /// tag table give every tag with "full": true; otherwise only the tags whose value, origin or forced
    /// state changed. Checks the image's versions first and reads every tag under one lock: a poll costs
    /// three atomic loads while the PLC is quiet, and never builds JSON for tags that did not change.
    std::optional<json> liveValues(const std::string& id, LiveState& state) const;
    json plcSummary(const Instance& instance) const;

    Engine* engine() { return engine_; }

private:
    std::shared_ptr<Instance> plc(const std::string& id) const;
    Engine& requireEngine() const;
    /// Changes the configuration of PLC `id` atomically (Engine::change): `edit` makes the next one from
    /// the current one and may throw ApiError. Refusals: 412 config_changed (stale version), 409
    /// tag_in_use (a running program would no longer compile), 422 (a task's program is missing or does
    /// not compile). `report` gets the tasks that restarted. Returns the PLC's id after the change.
    std::string changeConfig(const std::string& id, const std::function<PlcConfig(const PlcConfig&)>& edit,
                             const ChangeOptions& options, ReconfigureReport* report = nullptr);
    /// setupPlc() from the configuration `current`, applied only while the configuration has `version`.
    json setupFrom(const PlcConfig& current, const json& body, uint64_t version, const Principal* by);
    /// Events of a change of users, tokens or pairing ("access"), filed under this device's PLC.
    void accessEvent(const Principal* by, std::string message) const;
    /// The "program" event of a save: a draft, a download, or an online change with what it kept and reset.
    void logProgramEvent(const std::string& id, const SaveOutcome& save, bool deploy, const Principal* by) const;

    Engine* engine_;
    Auth& auth_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    /// When this process started (wall clock, ms): a fleet tells a restarted device by it.
    int64_t startedMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
    std::string webListen_ = "0.0.0.0";
    int webPort_ = 0;
};

/// JSON value of a tag: bool for bool tags, a number otherwise.
json tagValueJson(TagType type, double value);
/// A tag's value for the live stream: as tagValueJson, but a float32 that is NaN or infinite is the string
/// "NaN", "Infinity" or "-Infinity" (JSON has no such numbers; all three would be null and look alike).
json liveValueJson(TagType type, double value);
/// Who made a request, for events: a user, a token, or SimplePLC itself (none).
log::Author authorOf(const Principal* by);
/// Number for a tag write from JSON (true/false become 1/0); throws ApiError otherwise.
double jsonToValue(const json& value, const std::string& what);
/// A JSON number as a Structured Text literal (1e-05 becomes 1.0E-05: ST needs a decimal point
/// before an exponent), so any JSON number is accepted where ST literals are.
std::string numberLiteral(const json& number);

}  // namespace simpleplc::web
